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


// Helper function to reallocate memory across the entire system
void reallocateMemory(virConnectPtr conn, virDomainPtr* domains, int numDomains, unsigned long freeHostMemory) {
	// Define thresholds in KB
	const unsigned long MIN_DOMAIN_MEMORY = 100 * 1024;  // Minimum memory allowed for any domain (100 MB)
	const unsigned long HOST_FREE_MEMORY_THRESHOLD = 200 * 1024; // Minimum free memory that must remain on the host (200 MB)
	const unsigned long memAdjustStep = 64 * 1024;       // Memory adjustment step (64 MB)

	// Arrays to store domains that need memory and those that have excess memory
	MemoryStats needMemoryVms[numDomains];
	MemoryStats excessMemoryVms[numDomains];
	int needMemoryCount = 0, excessMemoryCount = 0;

	// Categorize each domain based on its unused memory
	for (int i = 0; i < numDomains; i++) {
		MemoryStats dStats = domainMemoryStats[i];

		// Debug: print current domain info
		printf("Domain %s: current memory = %lu KB, unused = %lu KB\n",
			virDomainGetName(dStats.domain), dStats.currentMem, dStats.unused);

		// If a domain's unused memory is less than the minimum threshold, it's considered "hungry"
		if (dStats.unused < MIN_DOMAIN_MEMORY) {
			needMemoryVms[needMemoryCount++] = dStats;
		}
		// If a domain's unused memory exceeds the minimum by at least the adjustment step, it can donate memory
		else if (dStats.unused > MIN_DOMAIN_MEMORY + memAdjustStep) {
			excessMemoryVms[excessMemoryCount++] = dStats;
		}
	}

	// First, try to pair domains that need memory with domains that can donate memory
	int i = 0, j = 0;
	while (i < needMemoryCount && j < excessMemoryCount) {
		MemoryStats* vmNeed = &needMemoryVms[i];
		MemoryStats* vmExcess = &excessMemoryVms[j];

		// Determine the transfer amount as the fixed adjustment step
		unsigned long transferAmount = memAdjustStep;

		// Adjust transferAmount so the donor does not drop below the minimum unused threshold
		if (vmExcess->unused < transferAmount + MIN_DOMAIN_MEMORY) {
			transferAmount = (vmExcess->unused > MIN_DOMAIN_MEMORY) ? vmExcess->unused - MIN_DOMAIN_MEMORY : 0;
		}

		if (transferAmount == 0) {
			// If donor cannot donate any memory, skip to next donor
			j++;
			continue;
		}

		// Check if the host has enough free memory to support the transfer
		if (freeHostMemory < transferAmount + HOST_FREE_MEMORY_THRESHOLD) {
			printf("Not enough free host memory for transfer. Breaking out.\n");
			break;
		}

		// Increase the memory for the needy VM
		unsigned long newMemNeed = vmNeed->currentMem + transferAmount;
		if (newMemNeed > vmNeed->maxMem) {
			newMemNeed = vmNeed->maxMem;
		}
		// Decrease the memory for the donor VM
		unsigned long newMemExcess = vmExcess->currentMem - transferAmount;
		if (newMemExcess < MIN_DOMAIN_MEMORY) {
			newMemExcess = MIN_DOMAIN_MEMORY;
		}

		// Attempt to set the new memory for both domains
		if (virDomainSetMemory(vmNeed->domain, newMemNeed) == 0 &&
			virDomainSetMemory(vmExcess->domain, newMemExcess) == 0) {
			printf("Adjusted memory: Domain %s increased to %lu KB, Domain %s decreased to %lu KB\n",
				virDomainGetName(vmNeed->domain), newMemNeed,
				virDomainGetName(vmExcess->domain), newMemExcess);
			// Update free host memory after the transaction
			freeHostMemory -= transferAmount;
			i++;
			j++;
		}
		else {
			fprintf(stderr, "Memory adjustment failed for %s or %s\n",
				virDomainGetName(vmNeed->domain), virDomainGetName(vmExcess->domain));
			break;
		}
	}

	// For any remaining needy VMs with no donors, try to increase their memory if host resources allow
	while (i < needMemoryCount && freeHostMemory >= memAdjustStep + HOST_FREE_MEMORY_THRESHOLD) {
		MemoryStats* vmNeed = &needMemoryVms[i];
		unsigned long newMemNeed = vmNeed->currentMem + memAdjustStep;
		if (newMemNeed > vmNeed->maxMem) {
			newMemNeed = vmNeed->maxMem;
		}
		if (virDomainSetMemory(vmNeed->domain, newMemNeed) == 0) {
			printf("Increased memory for domain %s to %lu KB\n",
				virDomainGetName(vmNeed->domain), newMemNeed);
			freeHostMemory -= memAdjustStep;
			i++;
		}
		else {
			fprintf(stderr, "Failed to increase memory for domain %s\n", virDomainGetName(vmNeed->domain));
			break;
		}
	}

	// For any remaining donor domains that have excess memory, reduce their allocation if possible
	while (j < excessMemoryCount) {
		MemoryStats* vmExcess = &excessMemoryVms[j];
		unsigned long newMemExcess = vmExcess->currentMem - memAdjustStep;
		if (newMemExcess < MIN_DOMAIN_MEMORY) {
			newMemExcess = MIN_DOMAIN_MEMORY;
		}
		if (virDomainSetMemory(vmExcess->domain, newMemExcess) == 0) {
			printf("Decreased memory for domain %s to %lu KB\n",
				virDomainGetName(vmExcess->domain), newMemExcess);
			freeHostMemory += memAdjustStep;
			j++;
		}
		else {
			fprintf(stderr, "Failed to reduce memory for domain %s\n", virDomainGetName(vmExcess->domain));
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
	reallocateMemory(conn, domains, numDomains, freeHostMemory);

 

	free(domains);
}