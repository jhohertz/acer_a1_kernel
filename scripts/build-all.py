#! /usr/bin/env python

# Copyright (c) 2009, Code Aurora Forum. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright
#       notice, this list of conditions and the following disclaimer in the
#       documentation and/or other materials provided with the distribution.
#     * Neither the name of Code Aurora nor
#       the names of its contributors may be used to endorse or promote
#       products derived from this software without specific prior written
#       permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
# OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
# WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
# OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
# ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# Build the kernel for all targets using the Android build environment.
#
# TODO: Accept arguments to indicate what to build.

import glob
from optparse import OptionParser
import subprocess
import os
import os.path
import shutil
import sys

version = 'build-all.py, version 0.01'

build_dir = '../all-kernels'
make_command = ["vmlinux", "modules"]
make_env = os.environ
make_env.update({
	'ARCH': 'arm',
	'CROSS_COMPILE': 'arm-eabi-',
	'KCONFIG_NOTIMESTAMP': 'true' })
all_options = {}

def fail(msg):
    """Fail with a user-printed message"""
    sys.stderr.write("error: %s\n" % msg)
    sys.exit(1)

def check_kernel():
    """Ensure that PWD is a kernel directory"""
    if (not os.path.isfile('MAINTAINERS') or
	    not os.path.isfile('arch/arm/mach-msm/Kconfig')):
	fail("This doesn't seem to be an MSM kernel dir")

def check_build():
    """Ensure that the build directory is present."""
    if not os.path.isdir(build_dir):
	fail("Build directory doesn't exist, please create: %s" %
		build_dir)

def update_config(file, str):
    print 'Updating %s with \'%s\'\n' % (file, str)
    defconfig = open(file, 'a')
    defconfig.write(str + '\n')
    defconfig.close()

def scan_configs():
    """Get the full list of defconfigs appropriate for this tree."""
    names = {}
    for n in glob.glob('arch/arm/configs/msm[0-9]*_defconfig'):
        names[os.path.basename(n)[:-10]] = n
    for n in glob.glob('arch/arm/configs/qsd*_defconfig'):
        names[os.path.basename(n)[:-10]] = n
    return names

class Builder:
    def __init__(self, logname):
	self.logname = logname
	self.fd = open(logname, 'w')

    def run(self, args):
	devnull = open('/dev/null', 'r')
	proc = subprocess.Popen(args, stdin=devnull,
		env=make_env,
		bufsize=0,
		stdout=subprocess.PIPE,
		stderr=subprocess.STDOUT)
	count = 0
	# for line in proc.stdout:
	rawfd = proc.stdout.fileno()
	while True:
	    line = os.read(rawfd, 1024)
	    if not line:
		break
	    self.fd.write(line)
	    self.fd.flush()
	    if all_options.verbose:
		sys.stdout.write(line)
		sys.stdout.flush()
	    else:
		for i in range(line.count('\n')):
		    count += 1
		    if count == 64:
			count = 0
			print
		    sys.stdout.write('.')
		sys.stdout.flush()
	print
	result = proc.wait()
	if result != 0:
	    fail("Failed to build, see %s" % self.logname)

    def close(self):
	self.fd.close()

def build(target):
    dest_dir = os.path.join(build_dir, target)
    log_name = '%s/log-%s.log' % (build_dir, target)
    print 'Building %s in %s log %s' % (target, dest_dir, log_name)
    if not os.path.isdir(dest_dir):
	os.mkdir(dest_dir)
    defconfig = 'arch/arm/configs/%s_defconfig' % target
    dotconfig = '%s/.config' % dest_dir
    shutil.copyfile(defconfig, dotconfig)

    devnull = open('/dev/null', 'r')
    subprocess.check_call(['make', 'O=%s' % dest_dir,
	'%s_defconfig' % target], env=make_env, stdin=devnull)
    devnull.close()

    if not all_options.updateconfigs:
        build = Builder(log_name)
        build.run(['make', 'O=%s' % dest_dir] + make_command)
        build.close()

    # Copy the defconfig back.
    if all_options.configs or all_options.updateconfigs:
        shutil.copyfile(dotconfig, defconfig)

def build_many(allconf, targets):
    print "Building %d target(s)" % len(targets)
    for target in targets:
	if all_options.updateconfigs:
            update_config(allconf[target], all_options.updateconfigs)
        build(target)

def main():
    check_kernel()
    check_build()

    configs = scan_configs()

    usage = ("usage: %prog [options] all\n" +
	"       %prog [options] target target ...")
    parser = OptionParser(usage=usage, version=version)
    parser.add_option('--configs', action='store_true',
	    dest='configs',
	    help="Copy configs back into tree")
    parser.add_option('--list', action='store_true',
	    dest='list',
	    help='List available targets')
    parser.add_option('-v', '--verbose', action='store_true',
	    dest='verbose',
	    help='Output to stdout in addition to log file')
    parser.add_option('--oldconfig', action='store_true',
	    dest='oldconfig',
	    help='Only process "make oldconfig"')
    parser.add_option('--updateconfigs',
            dest='updateconfigs',
            help="Update defconfigs with provided option setting, "
                 "e.g. --updateconfigs=\'CONFIG_USE_THING=y\'")
    parser.add_option('-j', '--jobs', type='int', dest="jobs", default=6,
	    help="Number of simultaneous jobs")

    (options, args) = parser.parse_args()
    global all_options
    all_options = options

    if options.list:
	print "Available targets:"
        for target in configs.keys():
	    print "   %s" % target
	sys.exit(0)

    if options.oldconfig:
	global make_command
	make_command = ["oldconfig"]

    make_command.append("-j%d" % options.jobs)

    if args == ['all']:
        build_many(configs, configs.keys())
    elif len(args) > 0:
	targets = []
	for t in args:
            if t not in configs.keys():
                parser.error("Target '%s' not one of %s" % (t, configs.keys()))
	    targets.append(t)
        build_many(configs, targets)
    else:
	parser.error("Must specify a target to build, or 'all'")

if __name__ == "__main__":
    main()
