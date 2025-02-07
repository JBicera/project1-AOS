# VCPU Scheduler 

Please write your algorithm and logic for the VCPU scheduler in this readme. Include details about how the algorithm is designed and explain how your implementation fulfills the project requirements. Use pseudocode or simple explanations where necessary to make it easy to understand.

Notes:
virConnectPtr(conn) - Points to connection object representing ocnnection to hypervisor
virConnectOpen() - Call to open connection to local system's hypervisor using "qemu:///system"
virConnectClose(conn) - Closes connection to clean up 
virConnectListAllDomains - Collects a possibly-filtered list of all domains and returns an allocated array of information for each
	Flags can be used to filter reuslts for smaller targeted domains
	Modifies array of virDomainPtr objects, each representing a domain
	Returns integer representing number of domains returned
	*Must allcoate domain of virDomainPtr** domains prior to call
virDomainPtr - Pointer to private structure representign a VM/Domain
	Contains all necessary information about a VM
	Used to interacting with VM such as starting,stopping, pausing
virDomainGetCPUStats - Retrieves CPU statistics for a domain
	*Use this for monitoring performance of the VM's CPU 
	Arguments(domain,cpuNum,params,nparams,flags)
		domain - Pointer to domain object
		cpuNum - CPU number to retrieve statistics for
		parms - Pointer to pre-allocated array of virTypedParameter structures to store the statistics
		nparams: Unsigned int of number of parameters returned
		flags: Modifies behavior of function
virDomainPinVcpu - Dynamically changes the real CPUs allocated to VCPUs
	Requires privilged access to hypervisor
	REturns 0 on success, -1 on failure
	Arugments(domain,vcpu,cpumap,maplen)
		domain - Pointer to domain(VM) object
		vcpu - Index of virtual CPU to pin
		cpumap - Pointer to a bitmask representing physical CPUs to which vCPU is pinned
		cpumaplen - Length of cpumap in BYTES
virDomainGetVcpus - Returns array of virVcpuInfo structures
	*Use VIR_COPY_CPUMAP macro to extract cpumap
	unsigned int cpumaplen
	unsigned char *cpumap = (unsigned char *)malloc(cpumaplen)
	VIR_COPY_CPUMAP(vcpu_info[vcpu_index].cpumaps, cpumaplen, vcpu_index, cpumap);


- Algorithm is independent of number of VCPUs and PCPUs
- Should be able to handle any number of either whether or not one is less than or greater than the other
- Generic approach is better than specific approach for all combinations
- Test cases expect 8 VPCUs and 4 PCPUs
- Balanced Schedule = No PCPU is under or overutilized
- Stable Schedule = Minimize necessary changes to VCPU-PCPU assignments, avoid frequent changes
- Absolute value of standard deviation should be <= 5 for CPU utilizations

Steps/To-Do List
1. Connect to Hypervisor !
2. List active virtual machines using virConnectList* functions !
3. Use virDomainGet* to get VCPU Statistics
4. Handle VCPU time data into usable format for calculations
5. Use virDomainGet* to identify current mapping(affinity) between VCPUs and PCPUs (Physical)
6. Develop algorithm based on statistics to assign the ebst PCPU for each VCPU, optimally schedule CPU usage
7. Update the VCPU assignment to its optimal CPU using virDomainPinVcpu


VPCU Scheduler Logic:
- Program is to optimize how VCPUs are assigned to PCPUs
- First get all the active domains/VMs 
- Iterate through the VCPU of each domain and collect its statistics
- Calculate how each is performing and store it to compare globally
	- This will require being able to store the VCPU ID (ID within its domain), Domain ID, Current PCPU, Cumulative CPU time, Previous CPU time, Utilization %)
	- Additionally for the PCPUs we need PCPU ID, Load (sum of utilization of all VCPUs of this PCPU), Number of assigned VCPUs
- Formula for VCPU utilization would be (Time used by VCPU during interval)/ (Total available CPU time during interval) * 100
- Use helper functions to caculate necessary PCPU and VCPU information
- My assumption is that 
