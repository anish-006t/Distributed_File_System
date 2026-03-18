# Multi-Computer Network Deployment Guide

## Quick Start - Subnet Deployment

This guide shows how to deploy the Distributed Document System across multiple computers on the same network subnet.

---

## Prerequisites

- ✅ All computers on same subnet (e.g., 192.168.1.0/24)
- ✅ Computers can ping each other
- ✅ Firewall allows required ports
- ✅ Same binary version on all computers

---

## Deployment Topology

```
Network: 192.168.1.0/24

┌────────────────────────────────────────────────────────┐
│                     Subnet                             │
│                                                        │
│  ┌──────────────┐  ┌──────────────┐  ┌─────────────┐ │
│  │ Computer A   │  │ Computer B   │  │ Computer C  │ │
│  │ .100         │  │ .101         │  │ .102        │ │
│  │ Name Server  │  │ Storage Srv  │  │ Storage Srv │ │
│  │ Port 5000    │  │ Port 6000    │  │ Port 6001   │ │
│  └──────────────┘  └──────────────┘  └─────────────┘ │
│                                                        │
│  ┌──────────────┐  ┌──────────────┐                  │
│  │ Computer D   │  │ Computer E   │                  │
│  │ .103         │  │ .104         │                  │
│  │ Client       │  │ Client       │                  │
│  └──────────────┘  └──────────────┘                  │
└────────────────────────────────────────────────────────┘
```

---

## Step-by-Step Setup

### Step 1: Prepare All Computers

**On each computer:**

```bash
# Download/copy the compiled binaries
# Ensure all have same version

# Verify binaries
ls -lh bin/
# Should see: client, nm, ss

# Make executable if needed
chmod +x bin/*
```

### Step 2: Configure Firewall

**On Name Server computer (192.168.1.100):**

```bash
# Ubuntu/Debian
sudo ufw allow 5000/tcp
sudo ufw status

# CentOS/RHEL
sudo firewall-cmd --add-port=5000/tcp --permanent
sudo firewall-cmd --reload

# Verify
sudo ss -tuln | grep 5000
```

**On Storage Server computers:**

```bash
# Computer B (port 6000)
sudo ufw allow 6000/tcp

# Computer C (port 6001)
sudo ufw allow 6001/tcp
```

### Step 3: Start Name Server

**Computer A (192.168.1.100):**

```bash
# Create data directory
mkdir -p /opt/dds/nm_data

# Start Name Server
cd /path/to/distributed-doc-system
./bin/nm 5000 /opt/dds/nm_data

# Leave this terminal open
# Should see no output (runs in foreground)
```

**Verify:**

```bash
# In another terminal on same computer
tail -f nm.log

# Should see:
# [2025-11-12 14:30:00] INFO NM listening on port 5000
```

**Test from another computer:**

```bash
# From Computer D
nc -zv 192.168.1.100 5000
# Should show: Connection succeeded
```

### Step 4: Start Storage Servers

**Computer B (192.168.1.101):**

```bash
# Create data directory
mkdir -p /opt/dds/ss_data

# Start Storage Server
./bin/ss 192.168.1.100 5000 6000 /opt/dds/ss_data

# Leave terminal open
```

**Verify:**

```bash
# Check SS log
tail -f ss.log

# Should see:
# [2025-11-12 14:31:00] INFO Registered to NM 192.168.1.100:5000 for port 6000
# [2025-11-12 14:31:00] INFO SS listening on 6000
```

**Computer C (192.168.1.102):**

```bash
# Create data directory
mkdir -p /opt/dds/ss_data

# Start Storage Server (note different port: 6001)
./bin/ss 192.168.1.100 5000 6001 /opt/dds/ss_data
```

**Verify Name Server sees both Storage Servers:**

```bash
# On Computer A, check NM log
tail nm.log

# Should see both registrations:
# [2025-11-12 14:31:00] INFO SS registered from 192.168.1.101:6000
# [2025-11-12 14:32:00] INFO SS registered from 192.168.1.102:6001
```

### Step 5: Connect Clients

**Computer D (192.168.1.103):**

```bash
./bin/client 192.168.1.100 5000

# Should see:
# Enter username: alice
# Connected to NM. Enter commands (VIEW, READ, CREATE, ...)
```

**Computer E (192.168.1.104):**

```bash
./bin/client 192.168.1.100 5000

# Enter username: bob
```

**Multiple clients from same computer:**

```bash
# Terminal 1
./bin/client 192.168.1.100 5000  # alice

# Terminal 2
./bin/client 192.168.1.100 5000  # charlie

# Terminal 3
./bin/client 192.168.1.100 5000  # diana
```

---

## Test Multi-Computer Deployment

### Test 1: File Creation and Access

**Client on Computer D (alice):**

```bash
> CREATE test-network.txt
File Created Successfully!

> WRITE test-network.txt 0
write> 1 Hello from Computer D
write> ETIRW
Write successful

> READ test-network.txt
Hello from Computer D
```

**Client on Computer E (bob):**

```bash
> VIEW -a
test-network.txt

> READ test-network.txt
ERR 4 Forbidden

# Bob doesn't have access yet
```

**Client on Computer D (alice):**

```bash
> ADDACCESS -R test-network.txt bob
Access granted successfully!
```

**Client on Computer E (bob):**

```bash
> READ test-network.txt
Hello from Computer D

# Now bob can read!
```

### Test 2: Concurrent Editing

**Client on Computer D (alice):**

```bash
> ADDACCESS -W test-network.txt bob

> WRITE test-network.txt 0
write> 2 from
write> 3 alice
write> ETIRW

> READ test-network.txt
Hello from alice
```

**Client on Computer E (bob) - simultaneously:**

```bash
> WRITE test-network.txt 1
write> 1 And hello from bob
write> ETIRW

> READ test-network.txt
Hello from alice
And hello from bob
```

**Both clients can edit different sentences at the same time!**

### Test 3: Storage Server Verification

**Verify file location:**

```bash
# On Computer B (first SS registered)
ls /opt/dds/ss_data/
# Should see: test-network.txt

# The file is stored on whichever SS was registered first
```

---

## Troubleshooting

### Problem: Client can't connect to Name Server

**Test connectivity:**

```bash
# From client computer
ping 192.168.1.100
nc -zv 192.168.1.100 5000
telnet 192.168.1.100 5000
```

**Solutions:**

1. **Verify NM is running:**
   ```bash
   # On Computer A
   ps aux | grep nm
   ss -tuln | grep 5000
   ```

2. **Check firewall:**
   ```bash
   sudo ufw status
   sudo ufw allow 5000/tcp
   ```

3. **Verify IP address:**
   ```bash
   # On Computer A
   ip addr show
   # Confirm IP is 192.168.1.100
   ```

### Problem: Storage Server won't register

**Check logs:**

```bash
# On Storage Server computer
tail -f ss.log

# Look for error messages
```

**Common issues:**

1. **Wrong NM IP:**
   ```bash
   # Verify you're using correct IP
   ping 192.168.1.100
   ```

2. **NM not started yet:**
   ```bash
   # Start NM first, wait 2 seconds, then start SS
   ```

3. **Port conflict:**
   ```bash
   # Check if port already in use
   ss -tuln | grep 6000
   
   # Use different port if needed
   ./bin/ss 192.168.1.100 5000 6002 /opt/dds/ss_data
   ```

### Problem: "File not found" but file was created

**Cause:** File created before Storage Server registered

**Solution:**

```bash
# Delete and recreate file
> DELETE oldfile.txt
> CREATE newfile.txt
```

### Problem: Slow performance

**Check network latency:**

```bash
# From client to NM
ping -c 10 192.168.1.100

# Should be < 10ms on same subnet
```

**Solutions:**

1. Use wired Ethernet (not WiFi)
2. Check network congestion
3. Verify computers on same subnet
4. Check switch/router performance

---

## Advanced Configurations

### Running Multiple Storage Servers on Same Computer

```bash
# Terminal 1
./bin/ss 192.168.1.100 5000 6000 /opt/dds/ss1

# Terminal 2
./bin/ss 192.168.1.100 5000 6001 /opt/dds/ss2

# Terminal 3
./bin/ss 192.168.1.100 5000 6002 /opt/dds/ss3
```

**Use case:** Testing, development, single-machine deployment

### Using Different Subnets (Advanced)

**Requirements:**
- Router configured to route between subnets
- Firewall allows cross-subnet traffic
- Higher latency expected

**Example:**

```
Subnet A: 192.168.1.0/24 (Name Server + SS)
Subnet B: 192.168.2.0/24 (Clients)

Router: 192.168.1.1 ↔ 192.168.2.1
```

**Client connection:**

```bash
# Client on Subnet B
./bin/client 192.168.1.100 5000

# Works if routing configured
```

### Docker Deployment (Future)

```yaml
version: '3'
services:
  nm:
    image: dds:latest
    command: ./bin/nm 5000 /data
    ports:
      - "5000:5000"
    volumes:
      - nm_data:/data
  
  ss1:
    image: dds:latest
    command: ./bin/ss nm 5000 6000 /data
    ports:
      - "6000:6000"
    volumes:
      - ss1_data:/data
  
  ss2:
    image: dds:latest
    command: ./bin/ss nm 5000 6001 /data
    ports:
      - "6001:6001"
    volumes:
      - ss2_data:/data
```

---

## Network Security

### Recommended Firewall Rules

**Name Server (192.168.1.100):**

```bash
# Only allow from known subnet
sudo ufw allow from 192.168.1.0/24 to any port 5000

# Deny all others
sudo ufw deny 5000/tcp
```

**Storage Servers:**

```bash
# Allow from clients
sudo ufw allow from 192.168.1.0/24 to any port 6000
```

### VPN Deployment

**For remote access:**

1. Set up VPN (OpenVPN, WireGuard)
2. Assign VPN clients to subnet
3. Connect clients through VPN
4. Same configuration as local subnet

---

## Performance Tuning

### Network Optimization

**Increase socket buffers:**

```bash
# On all computers
sudo sysctl -w net.core.rmem_max=16777216
sudo sysctl -w net.core.wmem_max=16777216
```

**Increase file descriptor limits:**

```bash
# In /etc/security/limits.conf
* soft nofile 4096
* hard nofile 8192
```

**Use Gigabit Ethernet:**
- 100 Mbps → ~12 MB/s max
- 1000 Mbps → ~125 MB/s max

### Storage Optimization

**Use SSD for Storage Servers:**
- HDD: ~100 IOPS
- SSD: ~10,000 IOPS
- NVMe: ~100,000 IOPS

**Mount with proper options:**

```bash
# /etc/fstab
/dev/sdb1 /opt/dds/ss_data ext4 noatime,nodiratime 0 0
```

---

## Monitoring

### Real-Time Log Monitoring

**Name Server:**

```bash
# On Computer A
tail -f nm.log | grep -E 'ERROR|Client|SS'
```

**Storage Servers:**

```bash
# On Computer B
tail -f ss.log | grep -E 'ERROR|WRITE|READ'
```

### Connection Monitoring

```bash
# See active connections to NM
sudo ss -tn | grep :5000

# See active connections to SS
sudo ss -tn | grep :6000
```

### Performance Monitoring

```bash
# Network traffic
ifstat -i eth0

# Disk I/O
iostat -x 1

# System load
htop
```

---

## Shutdown Procedure

**Graceful shutdown order:**

1. **Stop Clients** (Ctrl+C or QUIT command)
2. **Stop Storage Servers** (Ctrl+C)
3. **Stop Name Server** (Ctrl+C)

**Data preserved:**
- ✅ Name Server metadata (in nm_data/index.tsv)
- ✅ Storage Server files (in ss_data/)
- ✅ All logs

**Next startup:**
- Name Server loads metadata from disk
- Storage Servers re-register
- Files remain accessible

---

## Backup Strategy

**Name Server:**

```bash
# Backup metadata
tar czf nm-backup-$(date +%Y%m%d).tar.gz /opt/dds/nm_data
```

**Storage Servers:**

```bash
# Backup all files
tar czf ss-backup-$(date +%Y%m%d).tar.gz /opt/dds/ss_data
```

**Restore:**

```bash
# Stop servers
# Extract backups
tar xzf nm-backup-20251112.tar.gz -C /
tar xzf ss-backup-20251112.tar.gz -C /
# Start servers
```

---

## Summary

✅ **Network deployment is fully supported**  
✅ **Computers can be on different machines (same subnet)**  
✅ **Name Server, Storage Servers, and Clients can all be distributed**  
✅ **No code changes needed - already supports INADDR_ANY**  
✅ **Firewall configuration is the main requirement**

**Key Points:**
- Start Name Server first
- Configure firewalls on all computers
- Use actual IP addresses (not 127.0.0.1)
- Test connectivity with ping/nc before starting clients
- Monitor logs for troubleshooting

---

**Last Updated:** November 12, 2025  
**Deployment Status:** Production Ready for Subnet Deployment
