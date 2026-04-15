# Multi-Container Runtime with Kernel Memory Monitor

---

## 1. Team Information

**Team Members:**
- PES2UG24CS252: M.B. Divyadharshini  
- PES2UG24CS261: Manasi Sabnis  

---

## 2. Build, Load, and Run Instructions

### 🔧 Environment Setup

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)

Build the Project:
make

Load Kernel Module
sudo insmod monitor.ko

Verify Device Creation
ls -l /dev/container_monitor

Prepare Root Filesystem
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

Start Supervisor
sudo ./engine supervisor ./rootfs-base

Create Per-Container RootFS (New Terminal)
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta

Start Multiple Containers
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96

Check Container Status
sudo ./engine ps

View Container Logs (Logging Pipeline)
sudo ./engine logs alpha

Log files are stored in:

ls logs/
cat logs/alpha.log

Run Container in Foreground (Blocking Mode)
sudo ./engine run gamma ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
Stop using:
Ctrl + C

Stop Containers
sudo ./engine stop alpha
sudo ./engine stop beta

Memory Monitoring Test

Run a memory-heavy program:
sudo ./engine start memtest ./rootfs-alpha ./memhog --soft-mib 40 --hard-mib 60

Check logs:
sudo ./engine logs memtest

Check kernel messages:
dmesg | tail

Scheduling Experiment

Run CPU-bound workloads with different priorities:
sudo ./engine start c1 ./rootfs-alpha ./cpu_task --nice 0
sudo ./engine start c2 ./rootfs-beta ./cpu_task --nice 10

Cleanup

Stop all containers:

sudo ./engine stop alpha
sudo ./engine stop beta
sudo ./engine stop c1
sudo ./engine stop c2

Stop supervisor (Ctrl + C)
Unload kernel module:
sudo rmmod monitor


Demo with Screenshots

Each screenshot includes a short caption explaining what it demonstrates.

1. Multi-Container Supervision
Step1:
Starting supervisor:
$cd OS-Jackfruit/boilerplate
$sudo ./engine supervisor ../../images/rootfs-base


<img width="975" height="362" alt="image" src="https://github.com/user-attachments/assets/3e56ddf4-583a-435e-8db2-502653e3c9cd" />

Step2:
Starting multiple containers in another terminal.
$cd OS-Jackfruit/boilerplate
$sudo ./engine start alpha ../../images/rootfs-alpha “/bin/sleep 300”
$sudo ./engine start beta ../../images/rootfs-beta “/bin/sh”

<img width="972" height="192" alt="image" src="https://github.com/user-attachments/assets/70a458e1-8c2b-4d49-bdc2-0d5fc6c2f836" />

Supervisor console o/p:

<img width="977" height="112" alt="image" src="https://github.com/user-attachments/assets/5b0abc0a-0ff9-4392-b258-a1f4bda21ff5" />

Step3:
Checking container status:
$sudo ./engine ps

<img width="978" height="192" alt="image" src="https://github.com/user-attachments/assets/f15a8a92-69d9-4149-a73d-b10cf9f116cf" />

TASK2: Metadata Tracking

Step1:
Showing container status using “engine ps “command ,o/p contains ID,PID,STATE of container along with started timestamp.

<img width="977" height="192" alt="image" src="https://github.com/user-attachments/assets/955cc013-ecea-4bae-8d6e-01ef9b219070" />

3. Bounded-Buffer Logging

Step1:
Starting a container which produces a message every 5seconds.(producer)
$sudo ./engine start logger3 ../../images/rootfs-gamma “/bin/sh -c ‘counter=1;while true;do echo \”Counter value:\$counter\”;sleep 5;counter=\$((counter+1));done’”

<img width="975" height="122" alt="image" src="https://github.com/user-attachments/assets/299b29d2-8010-40fe-864d-ac4637a6e864" />

Step2:
Checking container status:
$sudo ./engine ps

<img width="976" height="117" alt="image" src="https://github.com/user-attachments/assets/a1f217a8-420f-4c6e-9898-fa9af508c1c4" />

Step3:
Checking logs using log command(consumer)
$sudo ./engine logs logger3

<img width="976" height="161" alt="image" src="https://github.com/user-attachments/assets/add1f885-4384-44ce-a019-23a26fbc6621" />

Step4:
Log file location
$sudo ls -l logs/

<img width="972" height="220" alt="image" src="https://github.com/user-attachments/assets/7697a119-acbd-4972-bcc1-519d7841e699" />

4. CLI and IPC:

Step1:
Container started using CLI
$sudo ./engine ps

<img width="978" height="120" alt="image" src="https://github.com/user-attachments/assets/76a3cec3-7fca-40ee-ad67-a61f665e9c35" />

Step2:
Supervisor responds

<img width="976" height="98" alt="image" src="https://github.com/user-attachments/assets/3c66f6ac-a677-4951-b173-69d0319499e2" />

Step3:
Container stopped using CLI:
$sudo ./engine stop logger3

<img width="976" height="102" alt="image" src="https://github.com/user-attachments/assets/e9e577cc-ac45-4a06-befb-85b49facc1ca" />

Supervisor response:

<img width="976" height="127" alt="image" src="https://github.com/user-attachments/assets/17ba6ef6-6a3a-4540-9dab-35afeaae2dda" />

Step4:
Blocking container started using run CLI command
$sudo ./engine run sigma ../../images/rootfs-sigma “/bin/sh”

<img width="977" height="80" alt="image" src="https://github.com/user-attachments/assets/0a183d60-644d-4f95-ae69-0de3babad587" />

<img width="975" height="221" alt="image" src="https://github.com/user-attachments/assets/2100fe71-b271-4c97-a5a1-a30e055553d2" />

TASK5:Soft-Limit Warning

Step1:
Memory hogging container started
$sudo ./engine start memory_hog_container ../../images/rootfs-alpha “/usr/bin/memory_hog” –soft-mib 30 –hard-mib 50

<img width="980" height="136" alt="image" src="https://github.com/user-attachments/assets/408459aa-f25b-4528-9f4a-4c3b49a0949a" />

Step2:
Memory usage of container using log command
$sudo ./engine logs memory_hog_container

<img width="977" height="225" alt="image" src="https://github.com/user-attachments/assets/4b550005-fa0e-4e84-a5c9-9f91ca9128ce" />

Soft limit warning from dmesg

<img width="977" height="92" alt="image" src="https://github.com/user-attachments/assets/286cd18d-5014-4e6d-b0e2-1a239be5ef71" />

TASK6:Hard-Limit Enforcement

Step1: Hard limit exceeded message through dmesg:

<img width="976" height="121" alt="image" src="https://github.com/user-attachments/assets/cae14377-68bb-4bd8-9b2e-b67c0f409ea0" />

Container getting killed automatically after warning:

<img width="977" height="121" alt="image" src="https://github.com/user-attachments/assets/07549f4b-1219-40cd-8658-6e9187d263d5" />

<img width="977" height="237" alt="image" src="https://github.com/user-attachments/assets/53cef942-6d30-424c-a0a7-9681c4ec1f0d" />

TASK8:Clean Teardown

<img width="971" height="172" alt="image" src="https://github.com/user-attachments/assets/751df4e6-03c0-4e1b-a659-a4b08109cb45" />

NOTE: Incase images are broken please refer to the documentation.

4. Engineering Analysis
 Isolation Mechanisms

The runtime uses:

PID namespace → isolates process IDs
UTS namespace → unique hostname per container
Mount namespace → separate filesystem

chroot() ensures containers only see their assigned filesystem.

Containers share:

Kernel
CPU scheduler
Memory subsystem
 Supervisor and Process Lifecycle

The supervisor:

Creates containers using clone()
Tracks metadata in linked list
Handles SIGCHLD to reap processes
Prevents zombie processes
 IPC, Threads, and Synchronization

IPC mechanisms:

Control → UNIX domain socket
Logging → pipes

Logging pipeline:

Producer → reads from pipes
Consumer → writes to log files

Synchronization:

Mutex for shared data
Condition variables for buffer control
 Memory Management and Enforcement

RSS:

Measures physical memory usage
Excludes swapped memory

Policies:

Soft limit → warning
Hard limit → kill

Kernel enforcement ensures reliability.

 Scheduling Behavior

Linux CFS scheduler:

Ensures fairness
Uses nice for priority

Lower nice → more CPU
Higher nice → less CPU

5. Design Decisions and Tradeoffs
Namespace Isolation
Choice: chroot + namespaces
Tradeoff: Less secure than pivot_root
Reason: Simpler implementation
Supervisor Architecture
Choice: Single supervisor
Tradeoff: Single point of failure
Reason: Easier management
IPC and Logging
Choice: Pipes + bounded buffer
Tradeoff: Limited buffer size
Reason: Prevents memory overflow
Kernel Monitor
Choice: Timer-based monitoring
Tradeoff: Periodic overhead
Reason: Simple and effective
Scheduling Experiment
Choice: nice
Tradeoff: Limited control
Reason: Easy and portable

<img width="790" height="343" alt="image" src="https://github.com/user-attachments/assets/28b0a195-6d87-4f60-8f1b-1c6a735e6479" />

<img width="792" height="262" alt="image" src="https://github.com/user-attachments/assets/ecfe57d9-4996-4811-97a0-05b2a19b0953" />

Conclusion
Lower nice → higher CPU priority
Higher nice → reduced CPU share
Scheduler balances fairness and responsiveness


Final Summary

This project demonstrates:

Multi-container runtime
Namespace isolation
IPC mechanisms
Thread synchronization
Kernel memory enforcement
Scheduling behavior analysis


































