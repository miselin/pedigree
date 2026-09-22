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

Power commands require root. They stop userspace, flush pending writes, and
cleanly unmount filesystems before the final power action. If the firmware cannot
switch the machine off, the screen displays **It is now safe to power off** and
the machine halts. You can then use the power button. `halt` reaches the same
screen without asking the firmware to switch off.

The safe message is withheld if storage teardown fails or a volume was already
unclean when mounted; shutdown does not repair filesystem damage. Reboot has
additional hardware reset fallbacks.

`/proc/meminfo` and `/proc/net/interfaces` expose the status reports as text.
The network link flag is the driver's reported state. Used memory includes
allocated cache pages; a reclaimable-cache estimate is not yet available.

The kernel log is bounded: older entries can be overwritten. Redirect it with
`dmesg > /tmp/kernel.log` to capture the current contents.
