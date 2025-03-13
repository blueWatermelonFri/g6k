cd kernel
make clean
make -j 16
cd ..
python setup.py clean
python setup.py build_ext -j 16 --inplace
