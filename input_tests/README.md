# How to get the gui running on cs machines from ssh:
1. Download TigetVNC
2. Build the test: `make -s -C /{your project dir}/user ../input_tests/input.dir/sbin/init`
3. From project directory: `bash /{your project dir}/display_qemu input`
4. In another terminal (not sshed): `ssh -N -L 127.0.0.1:5957:127.0.0.1:5957 {utcsid}@{your machine address}`
5. Connect to `127.0.0.1::5957` in TigerVNC
