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
	unsigned long prevUnused;
	unsigned long maxMem;
} MemoryStats;

// Global array to store memory stats for all domains
MemoryStats* domainMemoryStats = NULL;
float baselineFreeMemoryRatio = 0;


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
		// Initialize all values to 0 for the first run
		for (int i = 0; i < numDomains; i++) {
			domainMemoryStats[i].prevUnused = 0;
			domainMemoryStats[i].unused = 0;  // Initialize unused memory
			domainMemoryStats[i].currentMem = 0;
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
		// Preserve previous unused memory before updating it
		domainMemoryStats[i].prevUnused = domainMemoryStats[i].unused;
		domainMemoryStats[i].domain = domain;
		domainMemoryStats[i].maxMem = virDomainGetMaxMemory(domain);
		// Parse stats and store in our struct
		for (int j = 0; j < numStats; j++) {
			switch (stats[j].tag) 
			{
				case VIR_DOMAIN_MEMORY_STAT_UNUSED:
					domainMemoryStats[i].unused = stats[j].val;
					break;
				case VIR_DOMAIN_MEMORY_STAT_RSS:
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

// Function to dynamically reallocate memory for domains
void reallocateMemory(virConnectPtr conn, virDomainPtr* domains, int numDomains, unsigned long totalHostMemory, unsigned long freeHostMemory)
{
	float hostFreeRatio = (float)freeHostMemory / (float)totalHostMemory;
	const unsigned long MIN_VM_MEMORY = 100 * 1024;
	const float MEMORY_RATIO = 0.25;
	unsigned long newMemory;

	for (int i = 0; i < numDomains; i++)
	{
		virDomainPtr domain = domains[i];
		MemoryStats VMstats = domainMemoryStats[i];
		unsigned long currentMem = VMstats.currentMem;

		// Check if unused memory is decreasing
		int decreasingUnused = (VMstats.unused < VMstats.prevUnused);

		// Unused memory is decreasing/reaching minimum and host has memory to spare
		if ((VMstats.unused <= MIN_VM_MEMORY) || decreasingUnused) 
		{
			unsigned long maxMemory = VMstats.maxMem;
			// Host has at least 25% free memory to give out to VMs
			if (hostFreeRatio >= MEMORY_RATIO) 
			{
				// Cap memory to max limit
				if (newMemory > maxMemory) 
					newMemory = maxMemory; 
				newMemory = currentMem * (1 + MEMORY_RATIO);
				// Allocate memory back to VM
				if (virDomainSetMemory(domain, newMemory) == 0)
					printf("Increased memory for domain %d to %lu KB\n", i, newMemory);
				else
					fprintf(stderr, "Failed to increase memory for domain %d\n", i);
			}
		}
		// Unused memory is well above need and host is feeling memory pressure
		else if (VMstats.unused >= MIN_VM_MEMORY * 1.5 && !decreasingUnused && hostFreeRatio < MEMORY_RATIO)
		{
			newMemory = currentMem * (1 - MEMORY_RATIO);
			if (virDomainSetMemory(domain, newMemory) == 0)
				printf("Decreased memory for domain %d to %lu KB\n", i, newMemory);
			else
				fprintf(stderr, "Failed to decrease memory for domain %d\n", i);
		}
		else
			printf("VM is stable or idle. No need to reallocate\n");
	}
}

	

/*
COMPLETE THE IMPLEMENTATION
*/
void MemoryScheduler(virConnectPtr conn, int interval)
{
	virDomainPtr* domains = NULL;
	int numDomains = 0;
	unsigned long totalHostMemory;
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
	getHostMemoryStats(conn, &totalHostMemory, &freeHostMemory);

	// Call to reallocate memory
	reallocateMemory(conn, domains, numDomains, totalHostMemory, freeHostMemory);

 

	free(domains);
}