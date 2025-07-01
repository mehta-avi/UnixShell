#!/usr/bin/python
#
# Tests the functionality of history
#

import atexit, proc_check, time
from testutils import *

console = setup_tests()

# ensure that shell prints expected prompt
expect_prompt()

#test history builtin command
sendline("echo 1")
sendline("echo 2 | wc")
sendline("echo 3")
sendline("history")
expect_exact("1  echo 1\r\n2  echo 2 | wc\r\n3  echo 3\r\n4  history")

# test arrow keys
sendline("echo first")
sendline("echo second")
sendline("^[[A^[[A")
# expect_prompt()
expect_exact("first")
sendline("^[[A")
expect_exact("first")

# test expansion
# !n
sendline("!1")
expect_exact("1")
# !-n
sendline("echo a")
sendline("echo b")
sendline("echo c")
sendline("!-3")
expect_exact("a")
# !!
sendline("!!")
expect_exact("a")
# !string
sendline("!echo")
expect_exact("c")

test_success()