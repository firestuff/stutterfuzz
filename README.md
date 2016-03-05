# stutterfuzz

stutterfuzz is a fuzzer & stress tester for TCP servers. Using a directory
of input files to be sent, it rapidly opens many connections in parallel to a
single server and sends one file over each connection. It randomly varies
packet boundaries each time to attempt to trigger buffering & parsing issues
on the server.


## Input files

To generate input files for your server, you can start with some hand-crafted
client -> server protocol data, or dumps from existing connections. To expand
this test set to exercise more paths in your server code, consider using
[american fuzzy lop](http://lcamtuf.coredump.cx/afl/). Note that you may need
to modify your server to accept data from stdin to use afl. It's worth the
effort.


## Running the server

Connecting over loopback is best (for speed), so it's better if you can run
stutterfuzz and your server on the same machine. Consider making your server
[valgrind clean]() and running it under strict valgrind options:

```bash
valgrind --track-fds=yes --show-leak-kinds=all --leak-check=full ./yourserver --flag1 --flag2=value --dont-fork
```

This enables you to detect more cases of problems triggered by stutterfuzz.


## Running stutterfuzz

```bash
sudo apt-get -y install build-essential git clang
git clone https://github.com/flamingcowtv/stutterfuzz
cd stutterfuzz
make
./stutterfuzz --blob-dir=/path/to/source/directory --host=::1 --port=6789
```

stutterfuzz will print statistics about source files (check these for sanity),
then continously print runtime statistics.


## Stats

* `rounds`: number of passes through all input files
* `mean_cycles_to_connect`: number of cycle-ms length cycles we make to connect,
  average. > 1.00 here may indicate network latency, or that the server is
	overloaded and you should increase --cycle-ms or lower --num-conns
* `ready_to_send`: portion of attempts that a connection has received acks for
  all outgoing data and is ready to send more. < 1.00 here indicates network
	latency in excess of --cycle-ms, or delayed ack sending from the receiver.

Good statistics look like this:

```
Stats: rounds=7400, mean_cycles_to_connect=1.00, ready_to_send=1.00
```

Bad statistics look like this:

```
Stats: rounds=2400, mean_cycles_to_connect=68.08, ready_to_send=0.48            
```

## When will it stop?

stutterfuzz never exits normally; the search space is too large to be
deterministic. Hit ctrl-c to exit manually. It will also exit with an error
if any connections to the server fail, assuming that it has triggered a
problem (or is being blocked).
