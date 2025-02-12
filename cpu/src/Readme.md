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

Steps/To-Do List
1. Connect to Hypervisor !
2. List active virtual machines using virConnectList* functions !
3. Use virDomainGet* to get VCPU Statistics
4. Handle VCPU time data into usable format for calculations
5. Use virDomainGet* to identify current mapping(affinity) between VCPUs and PCPUs (Physical)
6. Develop algorithm based on statistics to assign the ebst PCPU for each VCPU, optimally schedule CPU usage
7. Update the VCPU assignment to its optimal CPU using virDomainPinVcpu


Repinning (repinVCPU()) Pseudocode
1. Collect Utilization Data
	- Retrieve VCPU utilization across all domains
	- Retrieve PCPu utilization by summing VCPU utilization for each PCPU 
2. Identify PCPU with most and least utilization
3. If difference between max and min utilization is above a threshold then proceed
	- If not don't repin
	- If yes then continue
4. Find most underutilized VCPU in the max PCPU
5. Create and allocate cpumap that points only to min PCPU
6. If the minmimum VCPU exists then repin it to the minimally utilized PCPU

Get VCPU Information Pseudocode
1. Get total VCPUs in the system
2. Allocate for VCPU info array if not already allocated
3. Iterate through all domains
	- For each VCPU in the domain get VCPU stats and store in struct
4. Free memory and return total VCPUs.

CPU Scheduler Pseudocode
1. Get list of active domains(VMs)
2. Get total number of VCPUs
3. Allocate memory for VCPU information array
4. Retrieve VCPU information using getVcpuInfo()
5. Repin using repinVcpus()
