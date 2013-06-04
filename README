FlogFS (Flash log filesystem)

Motivations:
I needed an efficient way to write to a raw NAND flash chip using minimal memory. I didn't need directories or the ability to write to arbitrary locations in a file. There seem to be no free designs out there capable of doing this in <10kB of RAM. I assume there are others like me that would also like such a thing.

Objectives:
FlogFS is intended to provide a very lightweight append-only filesystem for logging data on NAND flash chips. It should use only a few kB of RAM for each open file and should be only a few kB of ROM on most platforms. It is written in ANSI C and should be easily ported to any platform for different sizes of flash chips.

It is aware of the intricacies of the flash media and completely masks them to the application. Of course this isn't that absolute most efficient system in terms of data packing but tradeoffs must be taken to reduce CPU and memory load.

License:
A two-clause BSD license is applied to all code presented. See LICENSE


