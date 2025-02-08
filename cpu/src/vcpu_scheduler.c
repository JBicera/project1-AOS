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

/*
DO NOT CHANGE THE FOLLOWING FUNCTION
*/
void signal_callback_handler()
{
    printf("Caught Signal");
    is_exit = 1;
}

typedef struct {
    virDomainPtr domain; // Domain of VCPU
    int vcpuID; // The ID of the VCPU (useful for identifying the VCPU)
    int currentPcpu; // The current physical CPU the VCPU is pinned to
    unsigned long long prevCpuTime;  // Previous CPU time for utilization calculation
    unsigned long long currCpuTime;  // Current CPU time for utilization calculation
    double utilization;
} VcpuInfo;

/*
DO NOT CHANGE THE FOLLOWING FUNCTION
*/
int main(int argc, char* argv[])
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

// Helper Function: Get PCPU information and return total PCPUs
int getVcpuInfo(virDomainPtr* domains, int numDomains, VcpuInfo** vcpuInfo)
{
    // First, loop through all domains to count total VCPUs
    int totalVcpus = 0;

    for (int i = 0; i < numDomains; i++) {
        virVcpuInfoPtr vcpuInfoArray = NULL;

        // First, get the number of VCPUs
        int numVcpus = virDomainGetVcpus(domains[i], vcpuInfoArray, 0, NULL, 0);
        if (numVcpus < 0) {
            fprintf(stderr, "Error: Failed to get VCPU count for domain %d\n", i);
            continue;
        }

        totalVcpus += numVcpus;
    }

    if (totalVcpus == 0) {
        fprintf(stderr, "No VCPUs found for any domains.\n");
        return 0;
    }

    // Allocate memory for vcpuInfo to store details for each VCPU
    *vcpuInfo = (VcpuInfo*)calloc(totalVcpus, sizeof(VcpuInfo));
    if (!*vcpuInfo) {
        fprintf(stderr, "Error: Memory allocation failed for vcpuInfo\n");
        return 0;
    }

    // Now iterate through domains to collect VCPU and CPU stats data
    int vcpuIndex = 0;
    for (int i = 0; i < numDomains; i++) {
        // Get the number of VCPUs for this domain
        int numVcpus = virDomainGetVcpus(domains[i], NULL, 0, NULL, 0);
        if (numVcpus < 0) {
            fprintf(stderr, "Error: Failed to get VCPU count for domain %d\n", i);
            continue;
        }

        // Allocate memory to store VCPU info for this domain
        virVcpuInfoPtr vcpuInfoArray = (virVcpuInfoPtr)malloc(sizeof(virVcpuInfo) * numVcpus);
        if (!vcpuInfoArray) {
            fprintf(stderr, "Error: Memory allocation failed for vcpuInfoArray\n");
            continue;
        }

        // Call virDomainGetVcpus with a valid maxinfo value
        if (virDomainGetVcpus(domains[i], vcpuInfoArray, numVcpus, NULL, 0) < 0) {
            fprintf(stderr, "Error: Failed to get VCPU info for domain %d\n", i);
            free(vcpuInfoArray);
            continue;
        }

        // Get the CPU stats for the domain
        virTypedParameterPtr cpuStats = malloc(sizeof(virTypedParameter) * numVcpus);
        if (!cpuStats) {
            fprintf(stderr, "Error: Memory allocation failed for CPU stats\n");
            free(vcpuInfoArray);
            continue;
        }

        int nparams = 0;
        if (virDomainGetCPUStats(domains[i], NULL, 0, -1, 1, 0) < 0) {
            fprintf(stderr, "Error: Failed to get number of CPU stats parameters\n");
            free(vcpuInfoArray);
            free(cpuStats);
            continue;
        }

        // Retrieve CPU stats for the domain
        if (virDomainGetCPUStats(domains[i], cpuStats, nparams, 0, numVcpus, 0) < 0) {
            fprintf(stderr, "Error: Failed to get CPU stats for domain %d\n", i);
            free(vcpuInfoArray);
            free(cpuStats);
            continue;
        }

        // Populate the VcpuInfo array with VCPU stats data for each VCPU in the domain
        for (int j = 0; j < numVcpus; j++) {
            // Initialize prevCpuTime and currCpuTime for the first call
            if ((*vcpuInfo)[vcpuIndex].prevCpuTime == 0 && (*vcpuInfo)[vcpuIndex].currCpuTime == 0) {
                (*vcpuInfo)[vcpuIndex].prevCpuTime = vcpuInfoArray[j].cpuTime; // Initialize prevCpuTime
                (*vcpuInfo)[vcpuIndex].currCpuTime = vcpuInfoArray[j].cpuTime; // Initialize currCpuTime
                (*vcpuInfo)[vcpuIndex].vcpuID = vcpuInfoArray[j].number;
                (*vcpuInfo)[vcpuIndex].domain = domains[i];
            }
            else {
                // Update prevCpuTime and currCpuTime for subsequent calls
                (*vcpuInfo)[vcpuIndex].prevCpuTime = (*vcpuInfo)[vcpuIndex].currCpuTime; // Update prevCpuTime
                (*vcpuInfo)[vcpuIndex].currCpuTime = vcpuInfoArray[j].cpuTime; // Update currCpuTime
            }

            (*vcpuInfo)[vcpuIndex].currentPcpu = vcpuInfoArray[j].cpu;

            // Move to the next index in the vcpuInfo array
            vcpuIndex++;
        }

        // Free the temporary arrays
        free(vcpuInfoArray);
        free(cpuStats);
    }

    return totalVcpus;
}


// Helper function to get the number of physical CPUs.
int getNumPcpus(virConnectPtr conn) {
    virNodeInfo nodeInfo;
    if (virNodeGetInfo(conn, &nodeInfo) < 0) {
        fprintf(stderr, "Error: Failed to get node info\n");
        return 0;
    }
    return nodeInfo.cpus;
}

void repinVcpus(virConnectPtr conn, VcpuInfo* vcpuInfo, int totalVcpus, int interval, double threshold) {
    // Calculate utilization for each VCPU (percentage)
    // Utilization = ((currCpuTime - prevCpuTime) / interval) * 100.0
    for (int i = 0; i < totalVcpus; i++) {
        double util = ((double)(vcpuInfo[i].currCpuTime - vcpuInfo[i].prevCpuTime) / (double)interval) * 100.0;
        vcpuInfo[i].utilization = util;
    }

    // Get number of PCPUs
    int numPcpus = getNumPcpus(conn);
    if (numPcpus <= 0) {
        fprintf(stderr, "Error: No physical CPUs found.\n");
        return;
    }

    // Aggregate average utilization per PCPU
    double* totalUtil = (double*)calloc(numPcpus, sizeof(double));
    int* count = (int*)calloc(numPcpus, sizeof(int));
    for (int i = 0; i < totalVcpus; i++) {
        int p = vcpuInfo[i].currentPcpu;
        if (p >= 0 && p < numPcpus) {
            totalUtil[p] += vcpuInfo[i].utilization;
            count[p]++;
        }
    }
    double* avgUtil = (double*)calloc(numPcpus, sizeof(double));
    for (int i = 0; i < numPcpus; i++) {
        if (count[i] > 0)
            avgUtil[i] = totalUtil[i] / count[i]; // Compute the average utilization
        else
            avgUtil[i] = 0; // If no VCPUs are assigned, set utilization to zero
    }


    // Identify the max-loaded and min-loaded PCPUs
    int maxPcpu = 0, minPcpu = 0;
    for (int i = 1; i < numPcpus; i++) {
        if (avgUtil[i] > avgUtil[maxPcpu])
            maxPcpu = i;
        if (avgUtil[i] < avgUtil[minPcpu])
            minPcpu = i;
    }

    // Check if the difference exceeds the threshold.
    if (avgUtil[maxPcpu] - avgUtil[minPcpu] > threshold) {
        // Calculate cpumap length in bytes.
        unsigned int cpumapLen = (numPcpus + 7) / 8;
        // Allocate and zero out cpumap.
        unsigned char* cpumap = (unsigned char*)calloc(cpumapLen, sizeof(unsigned char));
        if (!cpumap) {
            fprintf(stderr, "Error allocating cpumap\n");
            free(totalUtil);
            free(count);
            free(avgUtil);
            return;
        }
        // Set the bit corresponding to the min-loaded PCPU.
        cpumap[minPcpu / 8] |= (1 << (minPcpu % 8));

        // Iterate over VCPUs on the max-loaded PCPU and repin those with high utilization.
        for (int i = 0; i < totalVcpus; i++) {
            if (vcpuInfo[i].currentPcpu == maxPcpu && vcpuInfo[i].utilization > avgUtil[maxPcpu]) {
                // Attempt to repin the VCPU to the min-loaded PCPU.
                int ret = virDomainPinVcpu(vcpuInfo[i].domain, vcpuInfo[i].vcpuID, cpumap, cpumapLen);

                if (ret < 0) {
                    fprintf(stderr, "Error: Failed to repin VCPU %d in its domain\n", vcpuInfo[i].vcpuID);
                }
                else {
                    printf("Repinned VCPU %d from PCPU %d to PCPU %d (Utilization: %.2f%%)\n",
                        vcpuInfo[i].vcpuID, maxPcpu, minPcpu, vcpuInfo[i].utilization);
                    // Update the currentPcpu field to reflect the new pinning.
                    vcpuInfo[i].currentPcpu = minPcpu;
                }
            }
        }
        free(cpumap);
    }

    free(totalUtil);
    free(count);
    free(avgUtil);
}


void CPUScheduler(virConnectPtr conn, int interval)
{
    virDomainPtr* domains;
    int numDomains = 0;
    VcpuInfo* vcpuInfo = NULL;
    int totalVcpus = 0;

    // List all active domains
    numDomains = virConnectListAllDomains(conn,&domains, VIR_CONNECT_LIST_DOMAINS_ACTIVE);
    if (domains == NULL) {
        fprintf(stderr, "Failed to list domains\n");
        return;
    }
    while (domains[numDomains] != NULL)
    {
        numDomains++;
    }

    // Get VCPU information
    totalVcpus = getVcpuInfo(domains, numDomains, &vcpuInfo); 
    
    // Run the repinning algorithm.
    repinVcpus(conn, vcpuInfo, totalVcpus, interval, 0.1);

    free(vcpuInfo);
    free(domains);
}