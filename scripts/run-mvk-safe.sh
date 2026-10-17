#!/bin/bash
# Fallback run for Xenos/Vulkan hangs or black screens on MoltenVK.
# Settings from goliathret/EdgeOfTimeRecomp src/platform/moltenvk.cpp (340144e).
export MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=1
export MVK_CONFIG_SEMAPHORE_SUPPORT_STYLE=3
export MVK_CONFIG_PRESENT_WITH_COMMAND_BUFFER=1
exec "$(dirname "$0")/run.sh" "$@"
