build_path = os.getenv("BUILDDIR")
package.cpath = build_path..'/test/box/?.so;'..build_path..'/test/box/?.dylib;'..package.cpath

cfunc = require('cfunc')
fio = require('fio')

cfunc_path = fio.pathjoin(build_path, "test/box/cfunc.so")
cfunc1_path = fio.pathjoin(build_path, "test/box/cfunc1.so")
cfunc2_path = fio.pathjoin(build_path, "test/box/cfunc2.so")

_ = pcall(fio.unlink(cfunc_path))
fio.symlink(cfunc1_path, cfunc_path)

--
-- Both of them are sitting in cfunc.so
cfunc.create('cfunc.cfunc_sole_call')
cfunc.create('cfunc.cfunc_fetch_array')

--
-- Make sure they are registered fine
cfunc.exists('cfunc.cfunc_sole_call')
cfunc.exists('cfunc.cfunc_fetch_array')

--
-- And they are listed
cfunc.list()

--
-- Make sure they all collable
cfunc.call('cfunc.cfunc_sole_call')
cfunc.call('cfunc.cfunc_fetch_array', {1,2,3,4})
-- This one should issue an error
cfunc.call('cfunc.cfunc_fetch_array', {1,2,4})

--
-- Clean old function references and reload new one.
_ = pcall(fio.unlink(cfunc_path))
fio.symlink(cfunc2_path, cfunc_path)

cfunc.reload('cfunc.cfunc_fetch_array')

cfunc.call('cfunc.cfunc_sole_call')
cfunc.call('cfunc.cfunc_fetch_array', {2,4,6})

--
-- Clean it up
cfunc.drop('cfunc.cfunc_sole_call')
cfunc.drop('cfunc.cfunc_fetch_array')

--
-- List should be empty
cfunc.list()

--
-- Cleanup the generated symlink
_ = pcall(fio.unlink(cfunc_path))
