# Memory Coordinator

This directory contains files for evaluating your Memory Coordinator. There are 3 test cases in total, each of which introduces a different workload.

## Understanding Memory coordinator Testing  
While designing your memory coordinator, there are a few things to consider:  
1. What are the kinds of memory consumption the coordinator needs to handle?  
2. What is the average case and what are the edge cases?
3. What statistics would help in making a decision to give/take memory?
4. When should you stop reclaiming memory?  
  
The test cases provided in this directory are by no means exhaustive, but are meant to be a sanity check for your memory coordinator. You are encouraged to think about other test cases while designing the coordinator.   

### Test Cases  
We provide three test cases as a starting point. Each test is described below, along with the rationale behind the tests. The setup for each test is the same — there are 4 VMs. All VMs start with 512 MB memory. We assume that the memory for each VM cannot fall below 200 MB. The max memory on each VM can grow up to 2048 MB. 

#### Test Case 1  
**Scenario**: There are 4 VMs, each with 512 MB. Only VM1 is running a program that consumes memory. Other VMs are idling.   

**Expected coordinator behavior**: VM1 keeps consuming memory until it hits the max limit. The memory is being supplied from the other VMs. Once VM1 hits the max limit, it starts freeing memory.  

**Sample output**:
```
Iteration number :  1
Memory (VM: aos_vm3)  Actual [512.0], Unused: [196.73046875]
Memory (VM: aos_vm4)  Actual [512.0], Unused: [197.2578125]
Memory (VM: aos_vm2)  Actual [512.0], Unused: [194.546875]
Memory (VM: aos_vm1)  Actual [512.0], Unused: [193.953125]
Iteration number :  2
Memory (VM: aos_vm3)  Actual [612.0], Unused: [174.37109375]
Memory (VM: aos_vm4)  Actual [415.0], Unused: [197.2890625]
Memory (VM: aos_vm2)  Actual [418.0], Unused: [194.546875]
Memory (VM: aos_vm1)  Actual [419.0], Unused: [193.953125]
...
Iteration number :  33
Memory (VM: aos_vm3)  Actual [2012.0], Unused: [813.83203125]
Memory (VM: aos_vm4)  Actual [415.0], Unused: [100.4453125]
Memory (VM: aos_vm2)  Actual [418.0], Unused: [100.76171875]
Memory (VM: aos_vm1)  Actual [418.0], Unused: [100.16796875]
Iteration number :  34
Memory (VM: aos_vm3)  Actual [2048.0], Unused: [771.84375]
Memory (VM: aos_vm4)  Actual [415.0], Unused: [100.4453125]
Memory (VM: aos_vm2)  Actual [418.0], Unused: [100.76171875]
Memory (VM: aos_vm1)  Actual [418.0], Unused: [100.16796875]
...
Iteration number :  46
Memory (VM: aos_vm3)  Actual [2048.0], Unused: [100.89453125]
Memory (VM: aos_vm4)  Actual [415.0], Unused: [100.2890625]
Memory (VM: aos_vm2)  Actual [418.0], Unused: [100.484375]
Memory (VM: aos_vm1)  Actual [418.0], Unused: [100.01171875]
...
Iteration number :  54
Memory (VM: aos_vm3)  Actual [2048.0], Unused: [1882.50390625]
Memory (VM: aos_vm4)  Actual [415.0], Unused: [100.2890625]
Memory (VM: aos_vm2)  Actual [418.0], Unused: [100.484375]
Memory (VM: aos_vm1)  Actual [418.0], Unused: [100.01171875]
Iteration number :  55
Memory (VM: aos_vm3)  Actual [1948.0], Unused: [1782.6015625]
Memory (VM: aos_vm4)  Actual [415.0], Unused: [100.2890625]
Memory (VM: aos_vm2)  Actual [418.0], Unused: [100.484375]
Memory (VM: aos_vm1)  Actual [418.0], Unused: [100.01171875]
...
```
- The first VM gains memory, which can be eithr supplied by other VMs(e.g iteration 2) or from the host (at iteration 33).   
- Once it reaches the max limit (iteration 34), the program that is consuming memory on the VM stops running after a while.
- In the meantime, the unused memory will keep going down (iteration 46).
- Once a large unused memory is observed, the coordinator begins reclaiming memory (iteration 54, 55 for example).

**What are we testing here?**  
This is the base case. The coordinator should be able to allocate/reclaim memory from a single VM.  


#### Test Case 2  
**Scenario**: There are 4 VMs with 512 MB each. All VMs begin consuming memory. They all have similar balloon sizes.  

**Expected coordinator behavior**: The coordinator decides if it can afford to supply more memory to the VMs, or if it should stop doing so. Once the VMs can no longer sustain the memory demands of the program running in them, the program stops running. Now the coordinator should begin reclaiming memory.   

**Sample output**:
```
Iteration number :  1
Memory (VM: aos_vm3)  Actual [512.0], Unused: [196.015625]
Memory (VM: aos_vm4)  Actual [512.0], Unused: [196.3203125]
Memory (VM: aos_vm2)  Actual [512.0], Unused: [193.09765625]
Memory (VM: aos_vm1)  Actual [512.0], Unused: [193.1015625]
Iteration number :  2
Memory (VM: aos_vm3)  Actual [612.0], Unused: [182.03515625]
Memory (VM: aos_vm4)  Actual [612.0], Unused: [181.56640625]
Memory (VM: aos_vm2)  Actual [612.0], Unused: [177.78515625]
Memory (VM: aos_vm1)  Actual [612.0], Unused: [177.45703125]
...
Iteration number :  32
Memory (VM: aos_vm3)  Actual [2012.0], Unused: [799.91796875]
Memory (VM: aos_vm4)  Actual [2012.0], Unused: [804.6953125]
Memory (VM: aos_vm2)  Actual [2012.0], Unused: [799.09765625]
Memory (VM: aos_vm1)  Actual [2012.0], Unused: [799.43359375]
Iteration number :  33
Memory (VM: aos_vm3)  Actual [2048.0], Unused: [800.64453125]
Memory (VM: aos_vm4)  Actual [2048.0], Unused: [803.84765625]
Memory (VM: aos_vm2)  Actual [2048.0], Unused: [799.33984375]
Memory (VM: aos_vm1)  Actual [2048.0], Unused: [800.40234375]
Iteration number :  34
Memory (VM: aos_vm3)  Actual [2048.0], Unused: [765.04296875]
Memory (VM: aos_vm4)  Actual [2048.0], Unused: [768.00390625]
Memory (VM: aos_vm2)  Actual [2048.0], Unused: [763.1328125]
Memory (VM: aos_vm1)  Actual [2048.0], Unused: [764.6796875]
...
Iteration number :  59
Memory (VM: aos_vm3)  Actual [2048.0], Unused: [1882.0625]
Memory (VM: aos_vm4)  Actual [2048.0], Unused: [1881.6875]
Memory (VM: aos_vm2)  Actual [2048.0], Unused: [1881.60546875]
Memory (VM: aos_vm1)  Actual [2048.0], Unused: [1881.75390625]
Iteration number :  60
Memory (VM: aos_vm3)  Actual [1948.0], Unused: [1791.0]
Memory (VM: aos_vm4)  Actual [1948.0], Unused: [1784.93359375]
Memory (VM: aos_vm2)  Actual [1948.0], Unused: [1785.69921875]
Memory (VM: aos_vm1)  Actual [1948.0], Unused: [1784.03125]
...
```
- All VMs start consuming memory, supplied by the host (iteration 2).   
- Their consumption is about the same. Once the coordinator determines that the host can no longer afford to give more memory (iteration 33), it stops supplying more memory.  
- The program on the VMs stops running and there is a large unusued memory, so the coordinator then starts reclaiming memory from the VMs (iterations 59, 60).

**What are we testing here?**  
When all VMs are consuming memory, we are forcing the coordinator to allocate memory from the host instead.   

#### Test Case 3  
**Scenario**: There are 4 VMs with 512 MB each. VM1 and VM2 initially start consuming memory. After some time, VM1 stops consuming memory but VM2 continues consuming memory. The other 2 VMs are inactive.   

**Expected coordinator behaviour**: The coordinator needs to decide how to supply memory to the VMs with growing demand.  

**Sample output**:  
```
Iteration number :  1
Memory (VM: aos_vm3)  Actual [512.0], Unused: [191.5546875]
Memory (VM: aos_vm4)  Actual [512.0], Unused: [191.57421875]
Memory (VM: aos_vm2)  Actual [512.0], Unused: [189.953125]
Memory (VM: aos_vm1)  Actual [512.0], Unused: [191.4375]
Iteration number :  2
Memory (VM: aos_vm3)  Actual [612.0], Unused: [179.6484375]
Memory (VM: aos_vm4)  Actual [612.0], Unused: [179.91015625]
Memory (VM: aos_vm2)  Actual [423.0], Unused: [189.953125]
Memory (VM: aos_vm1)  Actual [421.0], Unused: [191.4375]
...
Iteration 22:
Memory (VM: aos_vm3)  Actual [1512.0], Unused: [386.41015625]
Memory (VM: aos_vm4)  Actual [1512.0], Unused: [389.578125]
Memory (VM: aos_vm2)  Actual [422.0], Unused: [100.1328125]
Memory (VM: aos_vm1)  Actual [421.0], Unused: [100.52734375]
...
Iteration 33:
Memory (VM: aos_vm3)  Actual [1012.0], Unused: [691.4921875]
Memory (VM: aos_vm4)  Actual [1812.0], Unused: [744.23828125]
Memory (VM: aos_vm2)  Actual [422.0], Unused: [100.1328125]
Memory (VM: aos_vm1)  Actual [421.0], Unused: [100.52734375]
...
Iteration 40:
Memory (VM: aos_vm3)  Actual [512.0], Unused: [291.55078125]
Memory (VM: aos_vm4)  Actual [2048.0], Unused: [731.16015625]
Memory (VM: aos_vm2)  Actual [422.0], Unused: [100.1640625]
Memory (VM: aos_vm1)  Actual [421.0], Unused: [100.55859375]
...
Iteration number :  52
Memory (VM: aos_vm3)  Actual [421.0], Unused: [100.49609375]
Memory (VM: aos_vm4)  Actual [1948.0], Unused: [1780.62890625]
Memory (VM: aos_vm2)  Actual [422.0], Unused: [100.1953125]
Memory (VM: aos_vm1)  Actual [421.0], Unused: [100.58984375]
Iteration number :  53
Memory (VM: aos_vm3)  Actual [421.0], Unused: [100.49609375]
Memory (VM: aos_vm4)  Actual [1848.0], Unused: [1780.62890625]
Memory (VM: aos_vm2)  Actual [422.0], Unused: [100.1953125]
Memory (VM: aos_vm1)  Actual [421.0], Unused: [100.58984375]
...
```
- VM1 and VM2 are consuming memory initially (iteration2, iteration 22).  
- The coordinator supplies them more memory either by taking memory away from VM3 and VM4, or from the host.   
- Then VM1 stops consuming memory, but VM2's demand is still growing (iteration 33).  
- The coordinator can now redirect the memory from VM1 to VM2.  
- Then VM2 reaches its max limit (iteration 40).   
- Now the coordinator starts reclaiming memory from VM2 as well (iteration 52, 53).  
 
**What are we testing here?**  
We have a scenario where the memory demands are met by a combination of host memory and using the memory from idling VMs.


## Prerequisites for Testing

- Before starting these tests, ensure that you have created at least 4 virtual machines. 

- The names of these VMs **MUST** start with *aos_* (e.g., aos_vm1 for the first VM, aos_vm2 for the second VM, etc.).

- Note that if you have already created VMs as part of the CPU Scheduler tests, you can re-use 4 those VMs here but will need to delete the other 4. Otherwise, if you are starting with this portion of the assignment you will be creating 4 VMs here and another 4 VMs when you begin the CPU Scheduler portion.

- If you need to delete VMs, you can do so with the command: `uvt-kvm destroy aos_vm1` (where 'aos_vm1' is the name of the VM you wish to destroy).

- If you need to create VMs, you can do so with the command: `uvt-kvm create aos_vm1 release=bionic --memory 512` (where 'aos_vm1' is the name of the VM you wish to create).

- Ensure the VMs are shutdown before starting. You can do this with the script *~/project1/shutdownallvm.py*

Compile the test programs for the Memory Coordinator evaluations with the following commands:

```
$ cd ~/project1/memory/test/
$ ./makeall.sh
```
 
## Running The Tests
For each test, follow the procedure outlined below (using Test Case 1 as an example):

1. **Set the maximum allowed memory for VMs**  
   Run the script to configure the maximum memory for all VMs:  
   `~/project1/setallmaxmemory.py`

2. **Start all VMs**  
   Use the script to start all virtual machines:  
   `~/project1/startallvm.py`

3. **Copy test binaries to each VM**  
   Run the script to copy the required binaries:  
   `~/project1/memory/test/assignall.sh`

4. **Set up logging**  
   Open a new terminal (or a tmux/screen session) and capture terminal output using the `script` command:  
   `script memory_coordinator1.log`

5. **Start the monitoring tool**  
   In the same terminal, run the monitoring tool:  
   `~/project1/memory/test/monitor.py -t runtest1.py`  
   - The monitoring output will be logged in `memory_coordinator1.log` for submission.

6. **Run your Memory Coordinator**  
   Open a new terminal and execute the `memory_coordinator` binary.

7. **Verify correct behavior**  
   Use the monitoring tool output to ensure your Memory Coordinator behaves as described in:  
   `~/project1/README.md`

8. **Stop logging**  
   After the test completes, type `exit` to stop the `script` command.  
   - The log will be saved as `memory_coordinator1.log`.

9. **Move the log file**  
   Copy the log file to the directory:  
   `project1/memory/src/`

10. **Plot the test results**  
    Generate a graph of the test run using the plotting script:  
    `python plot_graph_memory.py -i <path to logfile> -o <path to save graph>`  
    - The generated graph is for self-review only and does not need to be submitted.
    - A sample graph is available [here](../../res/sample-sol-1.pdf).

11. **Shut down the VMs**  
    Shut down all virtual machines using the command:  
    `~/project1/shutdownallvm.py`

12. **Repeat for remaining test cases**  
    Repeat these steps for all test cases, updating the test case number as needed.


## Understanding Monitor Output

The *monitor.py* tool will output memory utilization statistics in the following format:

```
Iteration Number : 1
Memory (VM: aos_vm3)  Actual [512.0], Unused: [196.73046875]
Memory (VM: aos_vm4)  Actual [512.0], Unused: [197.2578125]
Memory (VM: aos_vm2)  Actual [512.0], Unused: [194.546875]
Memory (VM: aos_vm1)  Actual [512.0], Unused: [193.953125]
```

The first line mentions the iteration number. You should be able to see the expected behaviour in the test case within *100 iterations*. While running *~/project1/memory/test/monitor.py -t runtest1.py*, the test case *runtest1.py* starts after 5 iterations. That is, first 5 iterations just show you the monitor output without running the test case. Then, the script executes the test (in this case *runtest1.py*). 

Where "VM" is the VM for which statistics are printed, "Actual" is VM's total memory allocation in MB, and "Unused" is VM's current unused memory in MB.

