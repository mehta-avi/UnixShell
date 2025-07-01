#!/usr/bin/python
#
# Tests the functionality of our custom prompt built in
#

import atexit, proc_check, time
from testutils import *
import os
import socket

console = setup_tests()

username = os.getlogin()
server = socket.gethostname()
original_cwd = os.getcwd()
temp_cwd = original_cwd.rindex("/")
new_cwd = original_cwd[temp_cwd+1:]

final_string = username + "@" + server + " in " + new_cwd + "> "

expect_exact(final_string)

test_success()
