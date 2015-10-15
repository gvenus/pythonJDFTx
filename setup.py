# adapted from http://wiki.cython.org/PackageHierarchy
from __future__ import print_function

import sys, os, shutil, site
import multiprocessing
import subprocess as sb
import tempfile as tmp

from distutils.core import setup
from distutils.extension import Extension
from Cython.Distutils import build_ext
from Cython.Build import cythonize

isRoot = os.geteuid()==0 #Do we have root privileges?
enableGPU = (len(sys.argv)>=2 and sys.argv[1]=="--GPU")

# Use parallel compilation on this number of cores.
nthreads = int(os.environ.get('COMPILE_NTHREADS', multiprocessing.cpu_count() ))

class inTempFolder:
    def __enter__(self):
        self.currentDir = os.getcwd()
        self.tmpdir = tmp.mkdtemp()
        os.chdir(self.tmpdir)
        return self.tmpdir, self.currentDir
    def __exit__(self, type, value, traceback):
        shutil.rmtree(self.tmpdir)
        # pass

def installJDFTx():
    #is there a valid installation in user folders:
    if os.path.exists(os.path.join(site.USER_BASE, "jdftx/libjdftx.so")):
        return os.path.join(site.USER_BASE, "jdftx")
    with inTempFolder() as (jdftxDir, pythonJDFTxDir):
        print("Running cmake...")
        jdftxCodeDir = os.path.join(pythonJDFTxDir, "jdftx")
        if enableGPU:
            sb.check_call(["cmake", "-D", "EnableCUDA=yes",\
                            "-D", "EnableProfiling=yes", jdftxCodeDir])
        else:
            sb.check_call(["cmake", "-D", "EnableProfiling=yes", jdftxCodeDir])
        print("Running make. This takes a few minutes.")
        sb.check_call(["make", "-j%d"%nthreads])#, stderr = open("/dev/null") )
        if isRoot:
            jdftxLibDir = "/usr/local/jdftx"
        else:
            jdftxLibDir = os.path.join(site.USER_BASE, "jdftx")
        if not os.path.exists(jdftxLibDir):
            os.mkdir(jdftxLibDir)

        shutil.move("libjdftx.so", jdftxLibDir)
        if enableGPU:
            shutil.move("libjdftx_gpu.so", jdftxLibDir)
        shutil.move("pseudopotentials", jdftxLibDir)
        try:
            os.symlink("/usr/local/jdftx/libjdftx.so", \
                       "/usr/lib/libjdftx.so")
            if enableGPU:
                os.symlink("/usr/local/jdftx/libjdftx_gpu.so", \
                           "/usr/lib/libjdftx_gpu.so")
            return ""
        except OSError:
            return jdftxLibDir


#check if libjdftx is available
try:
    sb.check_call(["ld", "-ljdftx"])
    if enableGPU:
        sb.check_call(["ld", "-ljdftx_gpu"])
    jdftxLibDir = ""
except sb.CalledProcessError:
    jdftxLibDir = installJDFTx()

def make_extension(ext_name, ext_libraries=(), is_directory=False):
    ext_path = ext_name
    if is_directory:
        ext_path += ".__init__"
    return Extension(
        ext_name,
        [ext_path.replace(".", os.path.sep) + ".pyx"],
        include_dirs=(["jdftx", "."]),
        language="c++",
        libraries=ext_libraries,
        library_dirs=[jdftxLibDir],
        runtime_library_dirs=[jdftxLibDir],
        extra_compile_args=['-std=c++11'],
        #depends=["jdftx/libjdftx.so"],
    )

extensions = [
    make_extension("JDFTCalculator", ["jdftx"]),
]

setup(**{
    "name": "pythonJDFTx",
    "packages": [
        "core",
        "electronic",
        "includes",
        "fluid",
    ],
    "ext_modules": cythonize(extensions, nthreads=nthreads,compiler_directives = {'language_level':3}),
    "cmdclass": {'build_ext': build_ext},
})
