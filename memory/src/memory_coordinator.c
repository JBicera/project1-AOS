#include <stdio.h>
#include <stdlib.h>
#include <libvirt/libvirt.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#define MIN(a, b) ((a) < (b) ? a : b)
#define MAX(a, b) ((a) > (b) ? a : b)

int is_exit = 0; // DO NOT MODIFY THE VARIABLE

void MemoryScheduler(virConnectPtr conn, int interval);

// Define a struct to store only the necessary memory stats in KB
typedef struct {
	unsigned long unused;
	unsigned long available;
	unsigned long swapIn;
	unsigned long swapOut;
} MemoryStats;

// Global array to store memory stats for all domains
MemoryStats* domainMemoryStats = NULL;

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

	signal(SIGINT, signal_callback_handler);

	while (!is_exit)
	{
		// Calls the MemoryScheduler function after every 'interval' seconds
		MemoryScheduler(conn, interval);
		sleep(interval);
	}

	// Close the connection
	virConnectClose(conn);
	return 0;
}

// Helper Function: Enable memory statistics collection
int enableMemoryStats(virDomainPtr* domains,int numDomains, int period)
{
	for (int i = 0; i < numDomains; i++)
	{
		if (virDomainSetMemoryStatsPeriod(domains[i], period, 0) < 0)
		{
			fprintf(stderr, "Failed to set memory stats period for domain %d\n", i);
			return -1;
		}
	}
	return 1;
}
// Function to initialize and collect memory stats for all domains
int getMemoryStats(virDomainPtr* domains, int numDomains) 
{

	// Allocate memory for the stats array if not already allocated
	if (domainMemoryStats == NULL) 
	{
		domainMemoryStats = malloc(numDomains * sizeof(MemoryStats));
		if (!domainMemoryStats) 
		{
			fprintf(stderr, "Error: Memory allocation failed for domain memory stats\n");
			return -1;
		}
	}

	// Allocate temporary array for raw stats
	virDomainMemoryStatStruct stats[VIR_DOMAIN_MEMORY_STAT_NR];

	// Iterate over each domain and fetch stats
	for (int i = 0; i < numDomains; i++) 
	{
		virDomainPtr domain = domains[i];

		// Fetch memory stats
		int numStats = virDomainMemoryStats(domain, stats, VIR_DOMAIN_MEMORY_STAT_NR, 0);
		if (numStats == -1) {
			fprintf(stderr, "Error: Failed to get memory stats for domain %d\n", i);
			return -1;
		}

		// Parse stats and store in our struct
		for (int j = 0; j < numStats; j++) {
			switch (stats[j].tag) 
			{
				case VIR_DOMAIN_MEMORY_STAT_UNUSED:
					domainMemoryStats[i].unused = stats[j].val;
					break;
				case VIR_DOMAIN_MEMORY_STAT_AVAILABLE:
					domainMemoryStats[i].available = stats[j].val;
					break;
				case VIR_DOMAIN_MEMORY_STAT_SWAP_IN:
					domainMemoryStats[i].swapIn = stats[j].val;
					break;
				case VIR_DOMAIN_MEMORY_STAT_SWAP_OUT:
					domainMemoryStats[i].swapOut = stats[j].val;
					break;
				default:
					break; // Ignore other stats
			}
		}
	}
	return 1;
}

// Helper Function to get free memory of the system in KB
unsigned long getHostFreeMemory(virConnectPtr conn) 
{
	virNodeMemoryStatsPtr stats;
	int nstats = 0;
	int ret;
	unsigned long freeMemory = 0;

	// First call with stats == NULL to determine the number of stats available
	ret = virNodeGetMemoryStats(conn, VIR_NODE_MEMORY_STATS_ALL_CELLS, NULL, &nstats, 0);
	if (ret < 0) 
	{
		fprintf(stderr, "Failed to get memory stats count\n");
		return -1;  // Return -1 if the call fails
	}

	// Allocate an array for the memory statistics.
	stats = malloc(nstats * sizeof(virNodeMemoryStats));
	if (stats == NULL) 
	{
		fprintf(stderr, "Failed to allocate memory for host memory stats\n");
		return -1;  // Return -1 if allocation fails
	}

	// Retrieve the actual memory statistics.
	ret = virNodeGetMemoryStats(conn, VIR_NODE_MEMORY_STATS_ALL_CELLS, stats, &nstats, 0);
	if (ret < 0) 
	{
		fprintf(stderr, "Failed to get memory stats\n");
		free(stats);
		return -1;  // Return -1 if the retrieval fails
	}

	// Iterate over the stats and extract the "free" value.
	for (int i = 0; i < nstats; i++)
	{
		if (strcmp(stats[i].field, "free") == 0) 
		{
			freeMemory += stats[i].value; 
			break;  // Exit loop once free memory is found
		}
	}

	free(stats);  
	return freeMemory;  
}

// Helper function to reallocate memory across the entire system
void reallocateMemory(virConnectPtr conn, virDomainPtr* domains, int numDomains, unsigned long freeHostMemory) 
{
	// Define thresholds in KB
	unsigned long minVmUnused = 100 * 1024; // 100 MB minimum unused memory per VM
	unsigned long minFreeMemory = 200 * 1024; // 200 MB minimum unused memory for Host
	unsigned long memAdjustStep = 64 * 1024; // Step size for memory adjustment (64 MB)

	// Structs to store VMs with memory needs and excess
	MemoryStats needMemoryVms[numDomains];
	MemoryStats excessMemoryVms[numDomains];
	int needMemoryCount = 0, excessMemoryCount = 0;

	// For each domain, decide if we need to adjust memory allocation
	for (int i = 0; i < numDomains; i++) 
	{
		virDomainPtr domain = domains[i];
		// Get the memory stats for this domain from our global array
		MemoryStats dStats = domainMemoryStats[i];

		// Debug: print current domain info
		printf("Domain %s: current memory = %lu KB, unused = %lu KB, available = %lu KB, swapIn = %lu KB, swapOut = %lu KB\n",
			virDomainGetName(domain),
			virDomainGetMaxMemory(domain),
			dStats.unused,
			dStats.available,
			dStats.swapIn,
			dStats.swapOut);

		// Policy 1: If VM needs more memory and the host has enough free memory
		if ((dStats.unused < minVmUnused) || (dStats.swapIn > 0) || (dStats.swapOut > 0)) 
			needMemoryVms[needMemoryCount++] = dStats;
		
		// Policy 2: If VM has excess memory
		else if (dStats.unused > minVmUnused) 
			excessMemoryVms[excessMemoryCount++] = dStats;
		
	}

	int i = 0, j = 0;
	// First look for pairs of a VM that needs memory and VM that has excess
	while (i < needMemoryCount && j < excessMemoryCount) 
	{
		MemoryStats* vmNeedMemory = &needMemoryVms[i];
		MemoryStats* vmExcessMemory = &excessMemoryVms[j];

		// Determine the memory adjustment for both
		unsigned long newMemoryNeed = vmNeedMemory->available + memAdjustStep;
		unsigned long newMemoryExcess = vmExcessMemory->available - memAdjustStep;

		// Determine the memory adjustment for both
		unsigned long newMemoryNeed = currentMemoryNeed + memAdjustStep;
		unsigned long newMemoryExcess = currentMemoryExcess - memAdjustStep;

		// Ensure newMemoryExcess doesn't go below the minimum allowed for host
		if (newMemoryExcess < minVmUnused)
			newMemoryExcess = minVmUnused;

		// Only make changes if there is memory to faciliate the transfer
		if ((freeHostMemory - memAdjustStep >= minFreeMemory) && virDomainSetMemory(vmNeedMemory, newMemoryNeed) == 0 && virDomainSetMemory(vmExcessMemory, newMemoryExcess) == 0) 
		{
			printf("Adjusted memory: Domain %s increased to %lu KB, Domain %s decreased to %lu KB\n",
				virDomainGetName(vmNeedMemory->domain), newMemoryNeed,
				virDomainGetName(vmExcessMemory->domain), newMemoryExcess);

			// Update host free memory after the transaction
			freeHostMemory -= memAdjustStep;
			i++;
			j++;
		}
		else
			// If not enough free memory, break out of the loop
			break;
	}

	// IF there are more VMs needing memory than having excess memory
	while (i < needMemoryCount) 
	{
		MemoryStats* vmNeedMemory = &needMemoryVms[i];
		unsigned long currentMemoryNeed = virDomainGetMaxMemory(vmNeedMemory->domain);

		// Only proceed if host itself can supply the needed memory
		if (freeHostMemory - memAdjustStep >= minFreeMemory) 
		{
			unsigned long newMemoryNeed = currentMemoryNeed + memAdjustStep;
			if (virDomainSetMemory(vmNeedMemory->domain, newMemoryNeed) == 0)
			{
				printf("Increased memory for domain %s to %lu KB\n",
					virDomainGetName(vmNeedMemory->domain), newMemoryNeed);

				freeHostMemory -= memAdjustStep;
				i++;
			}
			else 
			{
				fprintf(stderr, "Failed to increase memory for domain %s\n", virDomainGetName(vmNeedMemory));
				break;
			}
		}
		else 
			break; // Host doesn't have enough memory to satisfy this need
	}

	// If there are more VMs with excess memory than VMs needing memory
	while (j < excessMemoryCount) 
	{
		MemoryStats* vmExcessMemory = &excessMemoryVms[j];
		unsigned long currentMemoryExcess = virDomainGetMaxMemory(vmExcessMemory->domain);

		// Decrease memory and reclaim it back to the host
		unsigned long newMemoryExcess = currentMemoryExcess - memAdjustStep;
		if (newMemoryExcess < minVmUnused)
			newMemoryExcess = minVmUnused;

		if (virDomainSetMemory(vmExcessMemory->domain, newMemoryExcess) == 0)
		{
			printf("Decreased memory for domain %s to %lu KB\n",
				virDomainGetName(vmExcessMemory->domain), newMemoryExcess);
			// Update host free memory after the transaction
			freeHostMemory += memAdjustStep;
			j++;
		}
		else 
		{
			fprintf(stderr, "Failed to decrease memory for domain %s\n", virDomainGetName(vmExcessMemory));
			break;
		}
	}
}



/*
COMPLETE THE IMPLEMENTATION
*/
void MemoryScheduler(virConnectPtr conn, int interval)
{
	virDomainPtr* domains = NULL;
	int numDomains = 0;
	unsigned long freeHostMemory;

	// Get list of all active domains
	numDomains = virConnectListAllDomains(conn, &domains, VIR_CONNECT_LIST_DOMAINS_ACTIVE);
	if (numDomains < 0 || domains == NULL)
	{
		fprintf(stderr, "Failed to list domains\n");
		return;
	}

	// Enable Memory Collection on all domains 
	if (enableMemoryStats(domains, numDomains, 0) < 0) // 0 for default hypervisor period
		fprintf(stderr, "Failed to enable memory stats\n"); 

	if (getMemoryStats(domains, numDomains) < 0) // Allocates for or allocates for global memory stats array
		fprintf(stderr, "Failed to get memory stats\n");

	// Get the amount of free memory the host has
	freeHostMemory = getHostFreeMemory(conn);
	if (freeHostMemory == -1)
		fprintf(stderr, "Failed to get host's free memory\n");

	// Call to reallocate memory
	reallocateMemory(conn, domains, numDomains, freeHostMemory);

 

	free(domains);
}