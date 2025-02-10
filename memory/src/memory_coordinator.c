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
	virDomainPtr domain;
	unsigned long currentMem;
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
		domainMemoryStats[i].domain = domain;
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
				case VIR_DOMAIN_MEMORY_STAT_ACTUAL_BALLOON:
					domainMemoryStats[i].currentMem = stats[j].val;
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

// New reallocation algorithm: Adjust memory by 20% increments/decrements
void reallocateMemory(virConnectPtr conn, virDomainPtr* domains, int numDomains, unsigned long freeHostMemory)
{
	const double ADJUST_PERCENTAGE = 0.25; // 25% gradual adjustment factor
	const unsigned long MIN_VM_MEMORY = 100 * 1024; // 100 MB minimum free memory for host
	const unsigned long MIN_HOST_FREE = 200 * 1024; // 200 MB minimum free memory for host

	// Iterate over all VMs to adjust memory allocations
	for (int i = 0; i < numDomains; i++)
	{
		virDomainPtr domain = domains[i];
		MemoryStats stats = domainMemoryStats[i];

		// Get max allowed memory for this VM
		unsigned long maxAllowed = virDomainGetMaxMemory(domain);
		if (maxAllowed == 0) 
		{
			fprintf(stderr, "Error: Failed to retrieve max memory for domain %s\n", virDomainGetName(domain));
			continue;
		}

		// Get current allocated memory from our pre-collected stats
		unsigned long currentMem = stats.currentMem;
		if (currentMem == 0) {
			fprintf(stderr, "Error: Current memory stats not available for domain %s\n", virDomainGetName(domain));
			continue;
		}

		// Calculate the used memory: used memory = currentMem - unused
		unsigned long usedMem = currentMem - stats.unused;

		// Decide if the VM needs more memory or less 
		// Needs more if unused is too little or is swapping in or out memory from disk
		if (usedMem > (currentMem * 0.75) || (stats.swapIn > stats.swapOut && stats.swapIn > 0))
		{
			unsigned long adjustAmount = (unsigned long)(currentMem * ADJUST_PERCENTAGE);

			// Calculate  new allocation
			unsigned long newMem = currentMem + adjustAmount;

			// Ensure we do not exceed the max allowed memory
			if (newMem > maxAllowed) {
				newMem = maxAllowed;
				adjustAmount = maxAllowed - currentMem; // Reduce the adjustment to fit within the max
			}

			// Ensure the host has enough free memory (Has at least 200 MB after transaction)
			if (freeHostMemory >= adjustAmount + MIN_HOST_FREE)
			{
				// Increase memory for VM
				if (virDomainSetMemory(domain, newMem) == 0)
				{
					printf("Increased memory for domain %s from %lu KB to %lu KB\n", virDomainGetName(domain), currentMem, newMem);
					freeHostMemory -= adjustAmount; // ADjust the allocated memory from host free memory
				}
				else
					fprintf(stderr, "Error: Failed to increase memory for domain %s\n",virDomainGetName(domain));
			}
			else
				printf("Insufficient host free memory to increase memory for domain %s\n",virDomainGetName(domain));
		}
		// Check if the VM has excess memory (Unused memory > 25% of current allocation)
		else if (stats.unused > (currentMem * ADJUST_PERCENTAGE))
		{
			// Swap-out activity could mean over-allocation, so we may reduce memory
			unsigned long adjustAmount = (unsigned long)(currentMem * ADJUST_PERCENTAGE);

			// Ensure that we do not reduce memory below the minimum allowed for a VM (100 MB)
			unsigned long newMem = currentMem - adjustAmount;
			// Ensure we do not reduce below the minimum allowed for a VM
			if (newMem < MIN_VM_MEMORY)
			{
				newMem = MIN_VM_MEMORY;
				adjustAmount = currentMem - MIN_VM_MEMORY; // Reclaim the actual memory difference
			}
			// Reclaim memory back to host
			if (virDomainSetMemory(domain, newMem) == 0)
			{
				printf("Decreased memory for domain %s from %lu KB to %lu KB\n",
					virDomainGetName(domain), currentMem, newMem);
				// Return the reclaimed memory to the host pool
				freeHostMemory += adjustAmount;
			}
			else
				fprintf(stderr, "Error: Failed to decrease memory for domain %s\n", virDomainGetName(domain));
		}
		else // No adjustment needed for this VM
			printf("No adjustment needed for domain %s (current: %lu KB, unused: %lu KB)\n",virDomainGetName(domain), currentMem, stats.unused);
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