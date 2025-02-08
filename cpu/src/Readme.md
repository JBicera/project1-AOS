# VCPU Scheduler 

Please write your algorithm and logic for the VCPU scheduler in this readme. Include details about how the algorithm is designed and explain how your implementation fulfills the project requirements. Use pseudocode or simple explanations where necessary to make it easy to understand.

Notes:
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
	Gives cumulative CPU time consumed by domain, CPU time for each indivdual VCPU, CPU time breakdown
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
	Use for getting cpumap, number of VCPUs assigned to the domain


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


What I need
- Need to every iteration of CPUscheduelr running get VCPU and PCPU information necessary to repin
- Repin it according to limiting under/overutilization
- Necessary Information
	- virDomainPinVcpu Info
		- Domain pointer to domain of VCPU you want to repin
		- vcpu index
		- Total number of PCPUs (Get from virNodeGetCPUMap)
		- Current cpumap of the VCPU
	- VCPU Utilization
		- Use virDomainGetCPUStats to retrieve metrics
		- Assign to custom struct to keep track for every individual VCPU


- Repinning Algorithm
	- Collect utilizaiton data from VCPUs and PCPUs from defined period
	- Calculate standrd deviation for each PCPU
	- Use this to calculate over/underutilized PCPUs
	- Assign VCPUs not using much CPU time to overutilized PCPUs and vice versa
		- For each underutilized PCPU: FInd an underutilized VCPU and pin it to that
		- For each overutilized PCPU: Find VCPU(s) pinned to it that can be repinned to an underutilized PCPU
	- Ensure sufficient capacity

Pseudocode/Workflow (CPU Scheduler)
1. Get list of domains(VMs)
2. Get total number of VCPUs 
3. Allocate memory for VCPU information array
4. Retrieve VCPU information from virDomainGetCPUStats() and store in struct
5. Calculate PCPU utilization using VCPU utilization in each PCPU
6. Perform repin if there is an overutilized PCPU and underutilized PCPU exceed the standard deviation
7. Repin using virDomainPinVcpu()
