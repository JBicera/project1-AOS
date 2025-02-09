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

typedef struct {
    virDomainPtr domain; // Domain of VCPU
    int vcpuID; // The ID of the VCPU (useful for identifying the VCPU)
    int currentPcpu; // The current physical CPU the VCPU is pinned to
    unsigned long long prevCpuTime;  // Previous CPU time for utilization calculation
    unsigned long long currCpuTime;  // Current CPU time for utilization calculation
    double utilization; // Utilization of VCPU
} VcpuInfo;

int is_exit = 0; // DO NOT MODIFY THIS VARIABLE
VcpuInfo* vcpuInfo = NULL; // Global VCPU array
int totalVcpus = 0; // Global total number of VCPUs

void CPUScheduler(virConnectPtr conn, int interval);
int getVcpuInfo(virDomainPtr* domains, int numDomains);
int getNumPcpus(virConnectPtr conn);

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
int getVcpuInfo(virDomainPtr* domains, int numDomains)
{
    // Get total VCPUs in system
    int totalVcpusTemp = 0;
    virDomainInfo info;
    for (int i = 0; i < numDomains; i++) 
    {
        if (virDomainGetInfo(domains[i], &info) < 0) 
        {
            fprintf(stderr, "Failed to get domain info\n");
            continue;
        }
        totalVcpusTemp += info.nrVirtCpu;
    }


    // Allocate memory for vcpuInfo only once
    if (vcpuInfo == NULL) 
    {
        vcpuInfo = (VcpuInfo*)calloc(totalVcpusTemp, sizeof(VcpuInfo));
        if (!vcpuInfo) 
        {
            fprintf(stderr, "Error: Memory allocation failed for vcpuInfo\n");
            return 0;
        }
        totalVcpus = totalVcpusTemp;
    }

    // Now iterate through domains to collect VCPU and CPU stats data
    int vcpuIndex = 0;
    for (int i = 0; i < numDomains; i++) 
    {
        // Get the number of VCPUs for this domain
        if (virDomainGetInfo(domains[i], &info) < 0) 
        {
            fprintf(stderr, "Failed to get domain info\n");
            continue;
        }
        int numVcpus = info.nrVirtCpu;

        // Allocate memory to store VCPU info for this domain
        virVcpuInfoPtr vcpuInfoArray = (virVcpuInfoPtr)malloc(sizeof(virVcpuInfo) * numVcpus);
        if (!vcpuInfoArray) 
        {
            fprintf(stderr, "Error: Memory allocation failed for vcpuInfoArray\n");
            continue;
        }

        // Call virDomainGetVcpus with a valid maxinfo value
        if (virDomainGetVcpus(domains[i], vcpuInfoArray, numVcpus, NULL, 0) < 0) 
        {
            fprintf(stderr, "Error: Failed to get VCPU info for domain %d\n", i);
            free(vcpuInfoArray);
            continue;
        }

        // Get available statistics number
        int nparams = virDomainGetCPUStats(domains[i], NULL, 0, -1, 1, 0);
        if (nparams < 0) 
        {
            fprintf(stderr, "Error: Failed to get number of CPU stats parameters\n");
            free(vcpuInfoArray);
            continue;
        }

        // Allocate the CPU stats array for the domain
        virTypedParameterPtr cpuStats = malloc(sizeof(virTypedParameter) * nparams);
        if (!cpuStats) 
        {
            fprintf(stderr, "Error: Memory allocation failed for CPU stats\n");
            free(vcpuInfoArray);
            continue;
        }

        // Retrieve CPU stats for the current domain
        if (virDomainGetCPUStats(domains[i], cpuStats, nparams, -1, 1, 0) < 0) 
        {
            fprintf(stderr, "Error: Failed to get CPU stats for domain %d\n", i);
            free(vcpuInfoArray);
            free(cpuStats);
            continue;
        }

        // Populate the VcpuInfo array with VCPU stats data for each VCPU in the domain
        for (int j = 0; j < numVcpus; j++) 
        {
            // First Time Initialization
            if (vcpuInfo[vcpuIndex].prevCpuTime == 0 && vcpuInfo[vcpuIndex].currCpuTime == 0) 
            {
                vcpuInfo[vcpuIndex].prevCpuTime = vcpuInfoArray[j].cpuTime;
                vcpuInfo[vcpuIndex].currCpuTime = vcpuInfoArray[j].cpuTime;
                vcpuInfo[vcpuIndex].vcpuID = j;
            }
            // Update CPU times on subsequent calls
            else 
            {
                vcpuInfo[vcpuIndex].prevCpuTime = vcpuInfo[vcpuIndex].currCpuTime;
                vcpuInfo[vcpuIndex].currCpuTime = vcpuInfoArray[j].cpuTime;
            }
            vcpuInfo[vcpuIndex].currentPcpu = vcpuInfoArray[j].cpu;
            vcpuInfo[vcpuIndex].domain = domains[i];
            vcpuIndex++;
        }

        // Free the temporary arrays
        free(vcpuInfoArray);
        free(cpuStats);
    }

    return totalVcpus;
}


// Helper function to get the number of physical CPUs.
int getNumPcpus(virConnectPtr conn) 
{
    virNodeInfo nodeInfo;
    if (virNodeGetInfo(conn, &nodeInfo) < 0) 
    {
        fprintf(stderr, "Error: Failed to get node info\n");
        return 0;
    }
    return nodeInfo.cpus;
}

// Helper function to repin CPUs if the usage difference is beyond a certain threshold
void repinVcpus(virConnectPtr conn, VcpuInfo* vcpuInfo, int totalVcpus, int interval, double threshold) {
    // Calculate utilization for each VCPU as a percentage
    // Utilization = ((currCpuTime - prevCpuTime) / (interval * 1e9)) * 100.0
    for (int i = 0; i < totalVcpus; i++) {
        double util = ((double)(vcpuInfo[i].currCpuTime - vcpuInfo[i].prevCpuTime) / (double)(interval * 1e9)) * 100.0;
        vcpuInfo[i].utilization = util;
    }

    // Get number of PCPUs
    int numPcpus = getNumPcpus(conn);
    if (numPcpus <= 0) {
        fprintf(stderr, "Error: No physical CPUs found.\n");
        return;
    }

    // Aggregate total utilization and count per PCPU
    double* totalUtil = (double*)calloc(numPcpus, sizeof(double));
    int* count = (int*)calloc(numPcpus, sizeof(int));
    for (int i = 0; i < totalVcpus; i++) {
        int p = vcpuInfo[i].currentPcpu;
        if (p >= 0 && p < numPcpus) {
            totalUtil[p] += vcpuInfo[i].utilization;
            count[p]++;
        }
    }

    // Print per-PCPU total utilizations
    printf("PCPU total utilizations:\n");
    for (int i = 0; i < numPcpus; i++) {
        printf("PCPU %d: %.2f%% (with %d VCPUs)\n", i, totalUtil[i], count[i]);
    }

    // Identify the most loaded and least loaded PCPUs
    int maxPcpu = 0, minPcpu = 0;
    for (int i = 1; i < numPcpus; i++) {
        if (totalUtil[i] > totalUtil[maxPcpu])
            maxPcpu = i;
        if (totalUtil[i] < totalUtil[minPcpu])
            minPcpu = i;
    }

    // If difference between max and min PCPU loads is greater than a certain threshold
    if (totalUtil[maxPcpu] - totalUtil[minPcpu] > threshold) 
    {
        int minVcpu = -1;
        double lowestVcpuUtil = 100.0;

        // For each VCPU on the most loaded PCPU: Find minimum
        for (int i = 0; i < totalVcpus; i++) {
            if (vcpuInfo[i].currentPcpu == maxPcpu || vcpuInfo[i].utilization < lowestVcpuUtil)
            {
                lowestVcpuUtil = vcpuInfo[i].utilization;
                minVcpu = i;
            }
        }

        // If a minimum VCPU exists
        if (minVcpu != -1) 
        {
            // Prepare cpumap that allows to allow for only min PCPU
            unsigned int cpumapLen = (numPcpus + 7) / 8;
            unsigned char* cpumap = (unsigned char*)calloc(cpumapLen, sizeof(unsigned char));
            if (!cpumap) {
                fprintf(stderr, "Error allocating cpumap\n");
                free(totalUtil);
                free(count);
                return;
            }
            cpumap[minPcpu / 8] |= (1 << (minPcpu % 8));

            // Repin min VCPU to min PCPU to redistribute
            int val = virDomainPinVcpu(vcpuInfo[minVcpu].domain, vcpuInfo[minVcpu].vcpuID, cpumap, cpumapLen);
            if (val < 0) {
                fprintf(stderr, "Error: Failed to repin VCPU %d from PCPU %d to PCPU %d\n",
                    vcpuInfo[minVcpu].vcpuID, maxPcpu, minPcpu);
            }
            else {
                printf("Repinned VCPU %d from PCPU %d to PCPU %d (Utilization: %.2f%%)\n",
                    vcpuInfo[minVcpu].vcpuID, maxPcpu, minPcpu, vcpuInfo[minVcpu].utilization);
                vcpuInfo[minVcpu].currentPcpu = minPcpu;  // Update the mapping
            }
            free(cpumap);
        }
        else {
            printf("No suitable candidate found for repinning.\n");
        }
    }
    else {
        printf("System is balanced, no repinning needed.\n");
    }
    free(totalUtil);
    free(count);
}



/* COMPLETE THE IMPLEMENTATION */
void CPUScheduler(virConnectPtr conn, int interval)
{
    virDomainPtr* domains = NULL;
    int numDomains = 0;

    // Get list of all active domains
    numDomains = virConnectListAllDomains(conn,&domains, VIR_CONNECT_LIST_DOMAINS_ACTIVE);
    if (numDomains < 0 || domains == NULL) 
    {
        fprintf(stderr, "Failed to list domains\n");
        return;
    }
    while (domains[numDomains] != NULL)
    {
        numDomains++; // Get domain count
    }

    // Get VCPU information
    totalVcpus = getVcpuInfo(domains, numDomains); 
    
    // Run the repinning algorithm.
    repinVcpus(conn, vcpuInfo, totalVcpus, interval, 10);

}