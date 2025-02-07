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

int is_exit = 0; // DO NOT MODIFY THIS VARIABLE
void CPUScheduler(virConnectPtr conn, int interval);

typedef struct {
	virDomainPtr domain;  // VM Reference
	int vcpuId;           // VCPU index
	int currentPCPU;      // Currently pinned PCPU
	uint64_t prevTime;    // Previous cumulative time
	uint64_t currTime;    // Current cumulative time
	double utilization;   // Percentage utilization
} vCPUInfo;

typedef struct {
	int pcpuId;          // PCPU index
	double totalLoad;    // Total CPU utilization from assigned VCPUs
	int numVcpus;        // Number of VCPUs assigned
} pCPUInfo;


/*
DO NOT CHANGE THE FOLLOWING FUNCTION
*/
void signal_callback_handler()
{
	printf("Caught Signal");
	is_exit = 1;
}

/*
DO NOT CHANGE THE FOLLOWING FUNCTION
*/
int main(int argc, char *argv[])
{
	virConnectPtr conn;

	if (argc != 2)
	{
		printf("Incorrect number of arguments\n");
		return 0;
	}

	// Gets the interval passes as a command line argument and sets it as the STATS_PERIOD for collection of balloon memory statistics of the domains
	int interval = atoi(argv[1]);

	conn = virConnectOpen("qemu:///system");
	if (conn == NULL)
	{
		fprintf(stderr, "Failed to open connection\n");
		return 1;
	}

	// Get the total number of pCpus in the host
	signal(SIGINT, signal_callback_handler);

	while (!is_exit)
	// Run the CpuScheduler function that checks the CPU Usage and sets the pin at an interval of "interval" seconds
	{
		CPUScheduler(conn, interval);
		sleep(interval);
	}

	// Closing the connection
	virConnectClose(conn);
	return 0;
}
// Helper Function: Calculate VCPU information
void calcVCPUInformation(vCPUInfo* vcpus, int numVCPUs, int interval) {
	// Only need one parameter cpu_time
	const int numParams = 1;

	// Iterate over all VCPUs
	for (int i = 0; i < numVCPUs; i++) {
		// Allocate an array for the stats on the stack (since numParams is constant)
		virTypedParameter params[numParams];

		// Get CPU stats for current VCPU (vcpus[i].vcpuId) and exit if unable to
		if (virDomainGetCPUStats(vcpus[i].domain, params, numParams, vcpus[i].vcpuId, 1, 0) == -1) {
			fprintf(stderr, "Error: Unable to get CPU stats for VCPU %d\n", vcpus[i].vcpuId);
			continue;
		}

		uint64_t previousTime = vcpus[i].currTime; // Save the previous cumulative time
		vcpus[i].currTime = params[0].value.ul; // Update current time

		// If previous time exist, calculate difference
		if (previousTime > 0) {
			uint64_t diff = vcpus[i].currTime - previousTime;
			// Utilization = 100 * VCPU time used during interval / Total available time for that vCPU durign that interval 
			vcpus[i].utilization = ((double)diff / (interval * 1000000000)) * 100.0;
		}
		else {
			// First run so utilization is still zero
			vcpus[i].utilization = 0.0;
		}
	}
}

// Helper Function: Calculate the PCPU load
void calcPCPULoad(vCPUInfo* vcpus, int numVCPUs, pCPUInfo* pcpus, int numPCPUs) {
	// Initialize values to zero
	for (int i = 0; i < numPCPUs; i++) {
		pcpus[i].totalLoad = 0.0;
		pcpus[i].numVcpus = 0;
	}
	// Increment values
	for (int i = 0; i < numVCPUs; i++) {
		int pcpuId = vcpus[i].currentPCPU;
		pcpus[pcpuId].totalLoad += vcpus[i].utilization;
		pcpus[pcpuId].numVcpus++;
	}
}

// Helper function to repin vCPUs to the least-loaded pCPUs
void repinVCPUs(vCPUInfo* vcpus, int numVCPUs, pCPUInfo* pcpus, int numPCPUs) {
    // Recalculate the load on each pCPU (reset and recount)
    for (int i = 0; i < numPCPUs; i++) {
        pcpus[i].totalLoad = 0.0;
        pcpus[i].numVcpus = 0;
    }
    for (int i = 0; i < numVCPUs; i++) {
        int assignedPCPU = vcpus[i].currentPCPU;
        pcpus[assignedPCPU].totalLoad += vcpus[i].utilization;
        pcpus[assignedPCPU].numVcpus++;
    }

    // Calculate average load and standard deviation
    double totalLoad = 0.0;
    for (int i = 0; i < numPCPUs; i++) {
        totalLoad += pcpus[i].totalLoad;
    }
    double avgLoad = totalLoad / numPCPUs;

    // Calculate the standard deviation of loads
    double sumSquaredDiffs = 0.0;
    for (int i = 0; i < numPCPUs; i++) {
        double diff = pcpus[i].totalLoad - avgLoad;
        sumSquaredDiffs += diff * diff;
    }
    double stdDev = sqrt(sumSquaredDiffs / numPCPUs);

    // Identify overutilized and underutilized PCPUs
    double overutilizedThreshold = avgLoad + stdDev;
    double underutilizedThreshold = avgLoad - stdDev;

    // Create lists of overutilized and underutilized PCPUs
    for (int i = 0; i < numVCPUs; i++) {
        int currentPCPU = vcpus[i].currentPCPU;
        double currentLoad = pcpus[currentPCPU].totalLoad;

        // If the current PCPU is overutilized or underutilized, attempt to repin
        if (currentLoad > overutilizedThreshold) {
            int leastLoadedPCPU = -1;
            double minLoad = FLT_MAX;

            // Look for an underutilized PCPU to repin the vCPU to
            for (int j = 0; j < numPCPUs; j++) {
                if (pcpus[j].totalLoad < underutilizedThreshold && pcpus[j].totalLoad < minLoad) {
                    leastLoadedPCPU = j;
                    minLoad = pcpus[j].totalLoad;
                }
            }

            // If a valid underutilized PCPU is found, repin vCPU
            if (leastLoadedPCPU != -1) {
                // Prepare a cpumap that allows only the least loaded pCPU.
                unsigned char cpumap[VIR_CPU_MAPLEN(numPCPUs)];
                memset(cpumap, 0, sizeof(cpumap));
                cpumap[leastLoadedPCPU / 8] |= (1 << (leastLoadedPCPU % 8));

                // Call virDomainPinVcpu to update the vCPU's pinning on the hypervisor
                if (virDomainPinVcpu(vcpus[i].domain, vcpus[i].vcpuId, cpumap, VIR_CPU_MAPLEN(numPCPUs)) == 0) {
                    // Update pCPU load information: remove load from the old pCPU and add to the new pCPU
                    pcpus[currentPCPU].totalLoad -= vcpus[i].utilization;
                    pcpus[currentPCPU].numVcpus--;
                    pcpus[leastLoadedPCPU].totalLoad += vcpus[i].utilization;
                    pcpus[leastLoadedPCPU].numVcpus++;

                    // Update the vCPU's current assigned pCPU
                    vcpus[i].currentPCPU = leastLoadedPCPU;
                    printf("Repinned vCPU %d from overutilized pCPU %d to underutilized pCPU %d\n", vcpus[i].vcpuId, currentPCPU, leastLoadedPCPU);
                }
                else {
                    fprintf(stderr, "Error: Failed to repin vCPU %d to pCPU %d\n", vcpus[i].vcpuId, leastLoadedPCPU);
                }
            }
        }
    }
}



/* COMPLETE THE IMPLEMENTATION */
void CPUScheduler(virConnectPtr conn, int interval) // conn = connection object, interval = how often to check
{
    static vCPUInfo* vcpus = NULL;
    static pCPUInfo* pcpus = NULL;
    static unsigned int numVCPUs = 0;
    static unsigned int numPCPUs = 0;

    virDomainPtr* domains;
    int numDomains;

    // List all domains
    int ret = virConnectListAllDomains(conn, &domains);
    if (ret < 0)
    {
        fprintf(stderr, "Error: Unable to list active domains\n");
        return;
    }
    // Initialize PCPUs if first time 
    if (numPCPUs == 0) {
        numPCPUs = virNodeGetCPUStats(conn, 0);
        if (numPCPUs < 0) {
            fprintf(stderr, "Error: Unable to retrieve number of PCPUs\n");
            return;
        }

        // Allocate PCPU array 
        pcpus = malloc(numPCPUs * sizeof(pCPUInfo));
        if (!pcpus) {
            fprintf(stderr, "Error: Memory allocation failed for PCPUs.\n");
            return;
        }

        // Initialize PCPU info to default
        for (int i = 0; i < numPCPUs; i++) {
            pcpus[i].pcpuId = i;
            pcpus[i].totalLoad = 0.0;
            pcpus[i].numVcpus = 0;
        }
    }

    // Initialize VCPUS if first time 
    if (numVCPUs == 0) {
        // Calculate number of VCPUs across all domains
        for (int i = 0; i < numDomains; i++) {
            virDomainPtr domain = domains[i];
            virDomainInfo info;

            if (virDomainGetInfo(domain, &info) != 0) {
                fprintf(stderr, "Error: Unable to get domain info for domain %d\n", i);
                continue;
            }
            numVCPUs += info.nrVirtCpu;  // Sum the number of vCPUs for this domain
        }

        // Allocate VCPU array
        vcpus = malloc(numVCPUs * sizeof(vCPUInfo));
        if (!vcpus) {
            fprintf(stderr, "Error: Memory allocation failed for VCPUS.\n");
            return;
        }

        // Initialize vCPU info to default
        int vcpuIndex = 0;
        for (int i = 0; i < numDomains; i++) {
            virDomainPtr domain = domains[i];
            virDomainInfo info;

            if (virDomainGetInfo(domain, &info) != 0) {
                fprintf(stderr, "Error: Unable to get info for domain %d\n", i);
                continue;
            }

            // For each vCPU in the current domain, initialize its information
            for (int j = 0; j < info.nrVirtCpu; j++) {
                vcpus[vcpuIndex].domain = domain;
                vcpus[vcpuIndex].vcpuId = j;
                vcpus[vcpuIndex].prevTime = 0;
                vcpus[vcpuIndex].currTime = 0;
                vcpus[vcpuIndex].utilization = 0.0;
                vcpus[vcpuIndex].currentPCPU = -1;

                // Retrieve the current PCPU for the vCPU
                unsigned char cpumap[VIR_CPU_MAPLEN(numPCPUs)];
                memset(cpumap, 0, sizeof(cpumap));
                int ret = virDomainGetVcpuPinInfo(domain, j, cpumap, VIR_CPU_MAPLEN(numPCPUs), 0);
                if (ret < 0) {
                    vcpus[vcpuIndex].currentPCPU = 0; // Default Value
                }
                else {
                    // Find the PCPU that the vCPU is currently pinned to
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
    calcVCPUInformation(vcpus, numVCPUs, interval);
    calcPCPULoad(vcpus, numVCPUs, pcpus, numPCPUs);
    repinVCPUs(vcpus, numVCPUs, pcpus, numPCPUs);

    // Free domain pointers
    for (int i = 0; i < numDomains; i++) {
        virDomainFree(domains[i]);
    }
    free(domains);
}

