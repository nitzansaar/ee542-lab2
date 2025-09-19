# Architecture of the Custom File Transfer Protocol

Our file transfer system is built on a **custom UDP-based reliable transport protocol** designed to handle packet loss, high latency, and bandwidth constraints. The architecture follows a **client-server model**, with multi-threaded execution on both sides to achieve efficient parallelism between sending, receiving, and retransmission logic.

---

## Client Side (Sender)

### Preload and Segmentation
- Reads the input file.
- Splits it into fixed-size packets (1024 bytes per packet, minus headers).
- Pushes packets into a buffer.

### Sender Thread
- Dedicated thread continuously transmits packets from a stack to the server over UDP.  
- Operates as a “fire-and-forget” worker, ensuring the entire file is initially transmitted as quickly as possible.

### NACK Receiver Thread
- Listens for **NACKs** (Negative Acknowledgments) from the server.  
- Reinserts requested packets into the stack for retransmission.  
- Stops when the server sends a completion signal (`NACK = -1`).

---

## Server Side (Receiver)

### Receiver Thread
- Accepts incoming packets and reassembles them into a **hash map** keyed by sequence number.  
- Detects missing sequence numbers and enqueues them into a **lost packet queue**.  
- Removes entries from the queue when retransmitted packets arrive.

### Sender Thread (NACK Generator)
- Periodically checks the lost packet queue.  
- If a packet hasn’t arrived within **2× RTT threshold**, sends a NACK to request retransmission.  
- Avoids wasting bandwidth with unnecessary acknowledgments.  
- Once all packets are received, sends a **completion signal** (`NACK = -1`).

---

## Data Structures

- **Reassembly Map**  
  `unordered_map<int, vector<char>>` for storing received packets by sequence number, enabling efficient in-order reconstruction.

- **Lost Packet Queue**  
  Custom `PacketQueue` with timestamping for retransmission timers.  
  - Prevents duplicate NACKs.  
  - Ensures fairness in packet recovery.

- **Packet Stack (Client)**  
  `stack<packet>` used to hold packets awaiting transmission or retransmission.

---

## Reliability and Verification

- After transfer completion, the server reassembles the file and computes an **MD5 checksum**.  
- Compares with the original file’s MD5 checksum for correctness.  
- Records **start and end timestamps** to measure throughput and evaluate protocol performance under varying RTT, loss, and MTU conditions.

---

## Summary

This design balances:
- **Speed**: Bulk UDP transmission.  
- **Reliability**: Selective NACK-based retransmission.  

By decoupling sender, receiver, and retransmission logic into separate threads and using efficient data structures, the protocol sustains **high throughput** even under **high RTT** and **packet loss** conditions.
