# Memory Coordinator

Please write your algorithm and logic for the memory coordinator in this readme. Include details about how the algorithm is designed and explain how your implementation fulfills the project requirements. Use pseudocode or simple explanations where necessary to make it easy to understand.

Notes:

int virDomainSetMemoryStatsPeriod(virDomainPtr domain,
                                  int period,
                                  unsigned int flags);

- domain - Pointer to the domain object representign the VM whose memory statistics collection period you want to set
- period - The collection interval in SECONDS, 0 is default hypervisor interval, -1 disables memory collection
- Returns 0 on success
- Too frequent memory collection = performance overhead

int virDomainMemoryStats(virDomainPtr dom,
                         virDomainMemoryStatPtr stats,
                         unsigned int nr_stats,
                         unsigned int flags);
- domain - Domain you want memory stats from
- stats - Array of virDomainMemoryStatStruct where memory statistics will be stored upon succesful execution
- nr_stats - Number of elements in stats array

int virDomainSetMemory(virDomainPtr domain, unsigned long memory);
- domain: Pointer to domain object representing a VM
- memory: Desired amount of memory to allocate to the domain
- This can be used to reduce or increase memory
- Set lower value to reduce, set higher value to allocate more

Relevant Flags
- VIR_DOMAIN_MEMORY_STAT_UNUSED - How much memory (bytes) is not being used by the VM that can be used for reallocation
- VIR_DOMAIN_MEMORY_STAT_AVAILABLE - Total usable memory (bytes) available to the VM
- VIR_DOMAIN_MEMORY_STAT_ACTUAL_BALLOON - Current size of VM's memory balloon
- VIR_DOMAIN_MEMORY_STAT_TOTAL - Total amount of memory allocated to the domain
- VIR_DOMAIN_MEMORY_STAT_SWAP_IN - Amount of memory moved from disk to RAM, High Value = Memory shortage
- VIR_DOMAIN_MEMORY_STAT_SWAP_OUT = Amount of memory moved from RAM to disk, High value = Memory pressure

Reminders:
Memory Ballooning - Technique to dynamically adjust the memory allocation of VMs based on their usage
- Each VM has a balloon driver that communicates with hypervisor
- Balloon inflates when host is running low on memory and blows up the balloon within a VM to effectively borrow memory 
- Memory allocated to the balloon drive is then reclaimed by the hypervisor and is made available to other VMs that might need it
- If the VM with the balloon starts to need more memory then it can "deflate" releasing the memory back to the VM to actually use

Steps
1. Conenct to Hypervisor 
2. List all active virtual machines
3. Enable memory statistics collection
- Iterate through each domain and call virDomaiNSetMemoryStatsPeriod to enable memory statistics collection
4. Retrieve Memory Statistics
5. Fetch Host memory Information
- Use virNodeGet* to ggather host memory details (Use this to inform your decision on allocating memory)
6. Design your algorithm
- Develop policy to allocate extra free memroy to each VM based on statistics
- Decide how much memory should be reserved and how much can be reallocated
- Don't reallocate memory too much, both VMs and host should have good enough emmory after releasing
- Release memory gradually
- Each VM should have 100 MB minimum of unused memory
- Host should have at least 200 MB, if equal to or less than do NOT release memory to VMs
7. Update Memory allocation using virDomainSetMemory 
8. Create a periodic memory scheduler
9. Test memory coordinator

Memory Reallocation Algorithm
1. For each VM make sure we have
- Unused free memory
- swap_in and swap_out to detect memory pressure
2. If a VM needs more memory and host has enough free memory then increase allocation
- If domain's unused memory < 100 MB, or
- If swap_in > 0 = Memory shortage, or
- If swap_out > 0 = Memory Pressure
- Store any VMs that satisfy any of these conditions
3. If VM has excess memory, reduce allocation 
- If domain's unused memory > 100 MB
- Store any VMs that satisfy any of these conditions
4. If there is enough memory, begin redistributing memory gradually among VMs that need it
- Start by reclaiming and releasing memory based off if there is a pair of VM that needs memory and VM that has excess
- If there are more memory starved VMs then release only if result of transaction leaves Host with > 200MB
- If there are more memor excess VMs then reclaim memory back to host