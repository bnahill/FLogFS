FLogFS (Flash Log File System)
===

Motivations:
---
I need an efficient way to write to a raw NAND flash chip using minimal memory. I don't need directories or random write access. There seem to be no free designs out there capable of doing this in <10kB of RAM. I assume there are others like me that would also like such a thing.

Objectives:
---
FlogFS is intended to provide a very lightweight append-only filesystem for logging data on SLC NAND flash chips. The development is centered around Micron's MT29F1 series SPI SLC NAND flash and a Cortex-M processor ([hardware](https://github.com/bnahill/wireless_logger "Wireless Logger")) but the design is meant to extend easily to many other configurations including MLC.

Features:
---
* Written in ANSI C11 (also valid C++11)
* Minimal memory footprint
	* ~600B (assuming 512B sector cache) of RAM for each open write file and should be <5kB of ROM/flash on most platforms
* Best-effort wear-leveling tracking block effort and allowing applications a per-file tradeoff of latency and wear-leveling effort
	* Background block allocation ("garbage collection") available to reduce average latency or to improve longevity
* Linked-list based organization for file blocks and inode tables
* The most recent non-atomic write operations (i.e. involving multiple blocks or sectors) are verified for completion upon mounting and cleaned up as needed to ensure consistency across interruption.
* Can make use of hardware or software ECC, though I didn't go and implement the software ECC. Maybe someday, but throughput would be crippled.

License:
---
A two-clause BSD license is applied to all code presented. See file 'LICENSE'


