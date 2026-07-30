// Copyright (c) 2026, Pedigree Developers
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

package main

import (
	"fmt"
	"io"
	"net"
	"os"
	"runtime"
	"time"
)

func main() {
	runtime.GOMAXPROCS(1)

	var output io.Writer = os.Stdout
	if console, err := os.OpenFile("/dev/console", os.O_WRONLY, 0); err == nil {
		defer console.Close()
		output = console
	}

	fmt.Fprintf(output, "GO-CANARY: start %s/%s\n", runtime.GOOS, runtime.GOARCH)

	data := make([]byte, 1<<20)
	for i := range data {
		data[i] = byte(i)
	}

	done := make(chan uint64)
	go func() {
		var checksum uint64
		for _, value := range data {
			checksum += uint64(value)
		}
		done <- checksum
	}()

	checksum := <-done
	fmt.Fprintf(output, "GO-CANARY: PASS scheduler checksum=%d\n", checksum)

	if os.Getenv("GO_CANARY_TIMER") == "1" {
		<-time.After(time.Millisecond)
		fmt.Fprintln(output, "GO-CANARY: PASS timer")
	}

	if os.Getenv("GO_CANARY_UNIX") == "1" {
		if err := checkUnixSocket(); err != nil {
			fmt.Fprintf(output, "GO-CANARY: FAIL unix socket: %v\n", err)
			os.Exit(1)
		}
		fmt.Fprintln(output, "GO-CANARY: PASS unix socket")
	}
}

func checkUnixSocket() error {
	const (
		path     = "/tmp/go-canary.sock"
		request  = "pedigree-go-unix-request"
		response = "pedigree-go-unix-response"
	)

	_ = os.Remove(path)
	listener, err := net.ListenUnix("unix", &net.UnixAddr{Name: path, Net: "unix"})
	if err != nil {
		return err
	}
	defer listener.Close()
	defer os.Remove(path)

	deadline := time.Now().Add(2 * time.Second)
	if err := listener.SetDeadline(deadline); err != nil {
		return err
	}

	serverDone := make(chan error, 1)
	go func() {
		connection, err := listener.AcceptUnix()
		if err != nil {
			serverDone <- err
			return
		}
		defer connection.Close()

		if err := connection.SetDeadline(deadline); err != nil {
			serverDone <- err
			return
		}

		buffer := make([]byte, len(request))
		if _, err := io.ReadFull(connection, buffer); err != nil {
			serverDone <- err
			return
		}
		if string(buffer) != request {
			serverDone <- fmt.Errorf("server received %q", buffer)
			return
		}
		if _, err := io.WriteString(connection, response); err != nil {
			serverDone <- err
			return
		}
		serverDone <- nil
	}()

	connection, err := net.DialUnix(
		"unix", nil, &net.UnixAddr{Name: path, Net: "unix"})
	if err != nil {
		return err
	}
	defer connection.Close()

	if err := connection.SetDeadline(deadline); err != nil {
		return err
	}
	if _, err := io.WriteString(connection, request); err != nil {
		return err
	}

	buffer := make([]byte, len(response))
	if _, err := io.ReadFull(connection, buffer); err != nil {
		return err
	}
	if string(buffer) != response {
		return fmt.Errorf("client received %q", buffer)
	}

	return <-serverDone
}
