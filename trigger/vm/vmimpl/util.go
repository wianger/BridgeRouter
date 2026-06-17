// Copyright 2017 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package vmimpl

import (
	"fmt"
	"time"

	"github.com/google/syzkaller/pkg/log"
	"github.com/google/syzkaller/pkg/osutil"
	"github.com/google/syzkaller/sys/targets"
)

// Sleep for d.
// If shutdown is in progress, return false prematurely.
func SleepInterruptible(d time.Duration) bool {
	select {
	case <-time.After(d):
		return true
	case <-Shutdown:
		return false
	}
}

func WaitForSSH(debug bool, timeout time.Duration, addr, sshKey, sshUser, OS string, port int, stop chan error) error {
	pwd := "pwd"
	if OS == targets.Windows {
		pwd = "dir"
	}
	connectTimeout := time.Minute
	startTime := time.Now()
	SleepInterruptible(5 * time.Second)
	for {
		select {
		case <-time.After(5 * time.Second):
		case err := <-stop:
			return err
		case <-Shutdown:
			return fmt.Errorf("shutdown in progress")
		}
		args := append(SSHArgsWithConnectTimeout(debug, sshKey, port, connectTimeout), sshUser+"@"+addr, pwd)
		if debug {
			log.Logf(0, "running ssh: %#v", args)
		}
		_, err := osutil.RunCmd(connectTimeout+30*time.Second, "", "ssh", args...)
		if err == nil {
			return nil
		}
		if debug {
			log.Logf(0, "ssh failed: %v", err)
		}
		if time.Since(startTime) > timeout {
			return &osutil.VerboseError{Title: "can't ssh into the instance", Output: []byte(err.Error())}
		}
	}
}

func SSHArgs(debug bool, sshKey string, port int) []string {
	return SSHArgsWithConnectTimeout(debug, sshKey, port, 10*time.Second)
}

func SSHArgsWithConnectTimeout(debug bool, sshKey string, port int, connectTimeout time.Duration) []string {
	return sshArgs(debug, sshKey, "-p", port, 0, connectTimeout)
}

func SSHArgsForward(debug bool, sshKey string, port, forwardPort int) []string {
	return sshArgs(debug, sshKey, "-p", port, forwardPort, 10*time.Second)
}

func SCPArgs(debug bool, sshKey string, port int) []string {
	return sshArgs(debug, sshKey, "-P", port, 0, 10*time.Second)
}

func sshArgs(debug bool, sshKey, portArg string, port, forwardPort int, connectTimeout time.Duration) []string {
	args := []string{
		portArg, fmt.Sprint(port),
		"-F", "/dev/null",
		"-o", "UserKnownHostsFile=/dev/null",
		"-o", "BatchMode=yes",
		"-o", "IdentitiesOnly=yes",
		"-o", "StrictHostKeyChecking=no",
		"-o", fmt.Sprintf("ConnectTimeout=%d", int(connectTimeout/time.Second)),
	}
	if sshKey != "" {
		args = append(args, "-i", sshKey)
	}
	if forwardPort != 0 {
		// Forward target port as part of the ssh connection (reverse proxy).
		args = append(args, "-R", fmt.Sprintf("%v:127.0.0.1:%v", forwardPort, forwardPort))
	}
	if debug {
		args = append(args, "-v")
	}
	return args
}
