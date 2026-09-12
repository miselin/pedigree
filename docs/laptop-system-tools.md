# System tools

| Command | Shows or does |
| --- | --- |
| `netconfig` | Network devices, addresses, MAC, MTU, default route, and DNS servers |
| `free -m` | Total, used, and free memory in MiB; `free` uses KiB |
| `dmesg` | The retained kernel log, without clearing it |
| `reboot` | Flush storage and restart |
| `poweroff` or `shutdown -h now` | Flush storage and switch off |
| `shutdown -r now` | Flush storage and restart |
| `halt` | Flush storage and halt; power remains on |

Power commands require root. Poweroff reports an error when firmware requires
ACPI sleep methods that Pedigree cannot execute. Reboot has additional hardware
reset fallbacks.

`/proc/meminfo` and `/proc/net/interfaces` expose the status reports as text.
The network link flag is the driver's reported state. Used memory includes
allocated cache pages; a reclaimable-cache estimate is not yet available.

The kernel log is bounded: older entries can be overwritten. Redirect it with
`dmesg > /tmp/kernel.log` to capture the current contents.
