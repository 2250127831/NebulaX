import sys

with open("src/tcp_server.cpp", "r") as f:
    content = f.read()

# Revert submit_and_wait_timeout(500) → submit_and_wait()
# This is the original behavior - no timeout SQE
content = content.replace(
    "int ret = poller_.submit_and_wait_timeout(500);",
    "int ret = poller_.submit_and_wait();"
)

# Also need to patch ShutdownGuard check since without timeout,
# the loop won't wake up periodically. But keep isStopping check
# so shutdown still works (will wait up to 500ms for next event)
# Actually, submit_and_wait() blocks until at least 1 CQE,
# so we need to keep the ShutdownGuard check but accept that
# shutdown might take longer.

with open("src/tcp_server.cpp", "w") as f:
    f.write(content)
print("OK")
