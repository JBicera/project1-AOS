#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <libvirt/libvirt.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <float.h>
#include <signal.h>
#define MIN(a, b) ((a) < (b) ? a : b)
#define MAX(a, b) ((a) > (b) ? a : b)

// DO NOT MODIFY THIS VARIABLE
int is_exit = 0;
void signal_callback_handler()
{
    printf("Caught Signal");
    is_exit = 1;
}

void CPUScheduler(virConnectPtr conn, int interval);

// Data structures – using capitalized names for consistency
typedef struct {
    virDomainPtr domain;  // Domain pointer 
    int vcpuId;           // VCPU index 
    int currentPCPU;      // Currently assigned PCPU
    uint64_t prevTime;    // Previous cumulative CPU time 
    uint64_t currTime;    // Current cumulative CPU time 
    double utilization;   // Calculated CPU utilization (in percent)
} VCPUInfo;

typedef struct {
    int pcpuId;          // PCPU index
    double totalLoad;    // Total CPU utilization from assigned VCPUs (in percent)
    int numVcpus;        // Number of VCPUs currently assigned to this PCPU
} PCPUInfo;

// --- Helper Functions ---

// Calculates VCPU utilization based on the change in cumulative CPU time.
void calcVCPUInformation(VCPUInfo* vcpus, int numVCPUs, int interval) {
    const int numParams = 1; // We only need one parameter: CPU time
    for (int i = 0; i < numVCPUs; i++) {
        virTypedParameter params[numParams];
        // Get CPU stats for current VCPU (using vcpuId)
        if (virDomainGetCPUStats(vcpus[i].domain, params, numParams, vcpus[i].vcpuId, 1, 0) == -1) {
            fprintf(stderr, "Error: Unable to get CPU stats for VCPU %d\n", vcpus[i].vcpuId);
            continue;
        }
        uint64_t previousTime = vcpus[i].currTime; // Save previous time
        vcpus[i].currTime = params[0].value.ul;      // Update current time from stats

        if (previousTime > 0) {
            uint64_t diff = vcpus[i].currTime - previousTime;
            // Calculate utilization: (diff / (interval in nanoseconds)) * 100
            vcpus[i].utilization = ((double)diff / (interval * 1000000000)) * 100.0;
        }
        else {
            vcpus[i].utilization = 0.0;
        }
    }
}

// Aggregates the utilization of VCPUs per physical CPU.
void calcPCPULoad(VCPUInfo* vcpus, int numVCPUs, PCPUInfo* pcpus, int numPCPUs) {
    // Initialize each PCPU info to zero.
    for (int i = 0; i < numPCPUs; i++) {
        pcpus[i].totalLoad = 0.0;
        pcpus[i].numVcpus = 0;
    }
    // Sum the utilization for each PCPU.
    for (int i = 0; i < numVCPUs; i++) {
        int pcpuId = vcpus[i].currentPCPU;
        if (pcpuId >= 0 && pcpuId < numPCPUs) {
            pcpus[pcpuId].totalLoad += vcpus[i].utilization;
            pcpus[pcpuId].numVcpus++;
        }
    }
}

// Attempts to repin VCPUs from overutilized PCPUs to underutilized ones.
void repinVCPUs(VCPUInfo* vcpus, int numVCPUs, PCPUInfo* pcpus, int numPCPUs) {
    // Recalculate PCPU load for current mapping
    for (int i = 0; i < numPCPUs; i++) {
        pcpus[i].totalLoad = 0.0;
        pcpus[i].numVcpus = 0;
    }
    for (int i = 0; i < numVCPUs; i++) {
        int assignedPCPU = vcpus[i].currentPCPU;
        if (assignedPCPU >= 0 && assignedPCPU < numPCPUs) {
            pcpus[assignedPCPU].totalLoad += vcpus[i].utilization;
            pcpus[assignedPCPU].numVcpus++;
        }
    }

    // Compute average load and standard deviation across PCPUs
    double totalLoad = 0.0;
    for (int i = 0; i < numPCPUs; i++) {
        totalLoad += pcpus[i].totalLoad;
    }
    double avgLoad = totalLoad / numPCPUs;
    double sumSquaredDiffs = 0.0;
    for (int i = 0; i < numPCPUs; i++) {
        double diff = pcpus[i].totalLoad - avgLoad;
        sumSquaredDiffs += diff * diff;
    }
    double stdDev = sqrt(sumSquaredDiffs / numPCPUs);

    double overutilizedThreshold = avgLoad + stdDev;
    double underutilizedThreshold = avgLoad - stdDev;

    // For each VCPU, if its current PCPU is overutilized, try to move it to a less-loaded one.
    for (int i = 0; i < numVCPUs; i++) {
        int currentPCPU = vcpus[i].currentPCPU;
        if (currentPCPU < 0 || currentPCPU >= numPCPUs)
            continue;
        double currentLoad = pcpus[currentPCPU].totalLoad;
        if (currentLoad > overutilizedThreshold) {
            int leastLoadedPCPU = -1;
            double minLoad = FLT_MAX;
            // Find an underutilized PCPU
            for (int j = 0; j < numPCPUs; j++) {
                if (pcpus[j].totalLoad < underutilizedThreshold && pcpus[j].totalLoad < minLoad) {
                    leastLoadedPCPU = j;
                    minLoad = pcpus[j].totalLoad;
                }
            }
            if (leastLoadedPCPU != -1) {
                // Prepare a cpumap that allows only the target PCPU.
                unsigned char cpumap[VIR_CPU_MAPLEN(numPCPUs)];
                memset(cpumap, 0, sizeof(cpumap));
                cpumap[leastLoadedPCPU / 8] |= (1 << (leastLoadedPCPU % 8));

                // Attempt to repin the VCPU
                if (virDomainPinVcpu(vcpus[i].domain, vcpus[i].vcpuId, cpumap, VIR_CPU_MAPLEN(numPCPUs)) == 0) {
                    printf("Repinned VCPU %d from PCPU %d to PCPU %d\n", vcpus[i].vcpuId, currentPCPU, leastLoadedPCPU);
                    vcpus[i].currentPCPU = leastLoadedPCPU;
                }
                else {
                    fprintf(stderr, "Error: Failed to repin VCPU %d to PCPU %d\n", vcpus[i].vcpuId, leastLoadedPCPU);
                }
            }
        }
    }
}

// This function is called repeatedly. It initializes VCPU and PCPU data only once though
void CPUScheduler(virConnectPtr conn, int interval)
{
    static VCPUInfo* vcpus = NULL;
    static PCPUInfo* pcpus = NULL;
    static unsigned int numVCPUs = 0;
    static unsigned int numPCPUs = 0;

    virDomainPtr* domains;
    int numDomains;

    // List all active domains; the return value is the number of domains.
    numDomains = virConnectListAllDomains(conn, &domains, 0);
    if (numDomains < 0) {
        fprintf(stderr, "Error: Unable to list active domains\n");
        return;
    }

    // --- Initialize PCPUs ---
    if (numPCPUs == 0) {
        int nparams = 0;
        // Use virNodeGetCPUStats to get the number of CPU stats parameters.
        if (virNodeGetCPUStats(conn, VIR_NODE_CPU_STATS_ALL_CPUS, NULL, &nparams, 0) < 0 || nparams == 0) {
            fprintf(stderr, "Error: Unable to retrieve number of CPU stats parameters\n");
            // Free domains before returning
            for (int i = 0; i < numDomains; i++) {
                virDomainFree(domains[i]);
            }
            free(domains);
            return;
        }
        // Allocate a temporary array to hold CPU stats.
        virNodeCPUStatsPtr stats = malloc(sizeof(virNodeCPUStats) * nparams);
        if (!stats) {
            fprintf(stderr, "Error: Memory allocation failed for CPU stats parameters\n");
            for (int i = 0; i < numDomains; i++) {
                virDomainFree(domains[i]);
            }
            free(domains);
            return;
        }
        memset(stats, 0, sizeof(virNodeCPUStats) * nparams);
        if (virNodeGetCPUStats(conn, VIR_NODE_CPU_STATS_ALL_CPUS, stats, &nparams, 0) < 0) {
            fprintf(stderr, "Error: Failed to get CPU stats\n");
            free(stats);
            for (int i = 0; i < numDomains; i++) {
                virDomainFree(domains[i]);
            }
            free(domains);
            return;
        }
        // Assume each parameter corresponds to one physical CPU.
        numPCPUs = nparams;
        free(stats);

        // Allocate and initialize the PCPU array.
        pcpus = malloc(numPCPUs * sizeof(PCPUInfo));
        if (!pcpus) {
            fprintf(stderr, "Error: Memory allocation failed for PCPUInfo array\n");
            for (int i = 0; i < numDomains; i++) {
                virDomainFree(domains[i]);
            }
            free(domains);
            return;
        }
        for (int i = 0; i < numPCPUs; i++) {
            pcpus[i].pcpuId = i;
            pcpus[i].totalLoad = 0.0;
            pcpus[i].numVcpus = 0;
        }
    }

    // --- Initialize VCPUs ---
    if (numVCPUs == 0) {
        // First, count the total number of VCPUs across all domains.
        for (int i = 0; i < numDomains; i++) {
            virDomainPtr domain = domains[i];
            virDomainInfo info;
            if (virDomainGetInfo(domain, &info) != 0) {
                fprintf(stderr, "Error: Unable to get domain info for domain %d\n", i);
                continue;
            }
            numVCPUs += info.nrVirtCpu;
        }
        // Allocate the VCPU array.
        vcpus = malloc(numVCPUs * sizeof(VCPUInfo));
        if (!vcpus) {
            fprintf(stderr, "Error: Memory allocation failed for VCPUInfo array\n");
            for (int i = 0; i < numDomains; i++) {
                virDomainFree(domains[i]);
            }
            free(domains);
            return;
        }
        // Initialize VCPU info.
        int vcpuIndex = 0;
        for (int i = 0; i < numDomains; i++) {
            virDomainPtr domain = domains[i];
            virDomainInfo info;
            if (virDomainGetInfo(domain, &info) != 0) {
                fprintf(stderr, "Error: Unable to get info for domain %d\n", i);
                continue;
            }
            for (int j = 0; j < info.nrVirtCpu; j++) {
                // Retain the domain pointer by increasing its reference count.
                vcpus[vcpuIndex].domain = virDomainRef(domain);
                vcpus[vcpuIndex].vcpuId = j;
                vcpus[vcpuIndex].prevTime = 0;
                vcpus[vcpuIndex].currTime = 0;
                vcpus[vcpuIndex].utilization = 0.0;
                vcpus[vcpuIndex].currentPCPU = -1;

                // Retrieve the current PCPU pinning for the vCPU.
                unsigned char cpumap[VIR_CPU_MAPLEN(numPCPUs)];
                memset(cpumap, 0, sizeof(cpumap));
                int ret = virDomainGetVcpuPinInfo(domain, j, cpumap, VIR_CPU_MAPLEN(numPCPUs), 0);
                if (ret < 0) {
                    // If no explicit pinning is configured, use the hypervisor’s default.
                    vcpus[vcpuIndex].currentPCPU = 0;
                }
                else {
                    // Otherwise, set the vCPU's current PCPU to the first allowed one.
                    for (int k = 0; k < numPCPUs; k++) {
                        if (cpumap[k / 8] & (1 << (k % 8))) {
                            vcpus[vcpuIndex].currentPCPU = k;
                            break;
                        }
                    }
                }
                vcpuIndex++;
            }
        }
    }

    // Now update VCPU utilization, PCPU load, and attempt repinning.
    calcVCPUInformation(vcpus, numVCPUs, interval);
    calcPCPULoad(vcpus, numVCPUs, pcpus, numPCPUs);
    repinVCPUs(vcpus, numVCPUs, pcpus, numPCPUs);

    // Free the domains list (domain pointers have been referenced already)
    for (int i = 0; i < numDomains; i++) {
        virDomainFree(domains[i]);
    }
    free(domains);
}
