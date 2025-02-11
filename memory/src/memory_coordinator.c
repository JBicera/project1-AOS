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
	unsigned long maxMem;
} MemoryStats;

// Global array to store memory stats for all domains
MemoryStats* domainMemoryStats = NULL;
unsigned long baselineFreeMemoryRatio = 0;

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
		domainMemoryStats[i].maxMem = virDomainGetMaxMemory(domain);
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

// Helper Function to get both total and free memory of the system in KB
void getHostMemoryStats(virConnectPtr conn, unsigned long* totalMemory, unsigned long* freeMemory)
{
	virNodeMemoryStatsPtr stats;
	int nstats = 0;
	int ret;

	// First call with stats == NULL to determine the number of stats available
	ret = virNodeGetMemoryStats(conn, VIR_NODE_MEMORY_STATS_ALL_CELLS, NULL, &nstats, 0);
	if (ret < 0)
	{
		fprintf(stderr, "Failed to get memory stats count\n");
		*totalMemory = 0;
		*freeMemory = 0;
		return;  // Return 0 if the call fails
	}

	// Allocate an array for the memory statistics.
	stats = malloc(nstats * sizeof(virNodeMemoryStats));
	if (stats == NULL)
	{
		fprintf(stderr, "Failed to allocate memory for host memory stats\n");
		*totalMemory = 0;
		*freeMemory = 0;
		return;  // Return 0 if allocation fails
	}

	// Retrieve the actual memory statistics.
	ret = virNodeGetMemoryStats(conn, VIR_NODE_MEMORY_STATS_ALL_CELLS, stats, &nstats, 0);
	if (ret < 0)
	{
		fprintf(stderr, "Failed to get memory stats\n");
		free(stats);
		*totalMemory = 0;
		*freeMemory = 0;
		return;  // Return 0 if the retrieval fails
	}

	// Initialize total and free memory values to 0
	*totalMemory = 0;
	*freeMemory = 0;

	// Iterate over the stats and extract the "total" and "free" values.
	for (int i = 0; i < nstats; i++)
	{
		if (strcmp(stats[i].field, "total") == 0)
		{
			*totalMemory += stats[i].value;
		}
		else if (strcmp(stats[i].field, "free") == 0)
		{
			*freeMemory += stats[i].value;
		}
	}
	free(stats);
}


// New reallocation algorithm: Adjust memory by 20% ratio
void reallocateMemory(virConnectPtr conn, virDomainPtr* domains, int numDomains, unsigned long totalHostMemory, unsigned long freeHostMemory) {
	
	// Define constants for memory adjustments and thresholds
	const unsigned long MIN_DOMAIN_MEMORY = 100 * 1024; // Minimum memory allowed for any domain (100 MB)
	const unsigned long HOST_FREE_MEMORY_THRESHOLD = 200 * 1024; // Threshold for free host memory (200 MB)
	const float MEMORY_RATIO = 0.2; // 20% deviation for memory adjustment


	// Calculate free memory ratio of the host system
	unsigned long currentFreeMemoryRatio = (float)freeHostMemory / totalHostMemory;
	printf("Current host memory ratio: %.2f\n", (float)currentFreeMemoryRatio);

	// Initialize baseline memory ratio if hasn't already
	// If currentFreeMemoryRatio < baselineMemoryRatio + MEMORY_RATIO = System has memory that is available to allocate
	// If currentFreeMemoryRatio > baselineMemoryRatio + MEMORY_RATIO = System is under memory pressure
	if (baselineFreeMemoryRatio == 0)
		baselineFreeMemoryRatio = currentFreeMemoryRatio;

	// Iterate over each domain to see if it either needs or has excess memory
	for (int i = 0; i < numDomains; i++)
	{
		unsigned long domainFreeMemRatio = (float)domainMemoryStats[i].available / domainMemoryStats[i].maxMem;

		// Ensure host system is above minimum and host's memory ratio is above the baseline threshold
		if (freeHostMemory > HOST_FREE_MEMORY_THRESHOLD && currentFreeMemoryRatio > baselineFreeMemoryRatio + MEMORY_RATIO) {
			unsigned long newMemory = MIN(domainMemoryStats[i].currentMem * (1 + MEMORY_RATIO), domainMemoryStats[i].maxMem);
			// Set new main memory to the ratio increase OR the max amount allowed if newMemory is over
			printf("Domain %d: Reallocating memory to %lu KB\n", i, newMemory);
			virDomainSetMemory(domainMemoryStats[i].domain, newMemory);
		}

		// If domains have excess memory
		else if (domainFreeMemRatio > MEMORY_RATIO)
		{
			// Domain has excess memory, so reduce the memory of the domain but not below 100 MB
			unsigned long newMemory = MAX(domainMemoryStats[i].currentMem * (1 - MEMORY_RATIO), MIN_DOMAIN_MEMORY);
			printf("Domain %d: Reducing memory to %lu KB\n", i, newMemory);
			virDomainSetMemory(domainMemoryStats[i].domain, newMemory);
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
	unsigned long totalHostMemory = 0, freeHostMemory = 0;

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
	getHostMemoryStats(conn, &totalHostMemory, &freeHostMemory);
	if (totalHostMemory == 0 || freeHostMemory == 0) {
		fprintf(stderr, "Failed to get valid host memory stats\n");
		return;
	}

	// Call to reallocate memory
	reallocateMemory(conn, domains, numDomains, totalHostMemory, freeHostMemory);

 

	free(domains);
}