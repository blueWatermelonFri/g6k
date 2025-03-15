# 如果要编译fpylll里面的代码，例如/home/heyuanhong/project/g6k/g6k-fpylll/src/fpylll/fplll/gso.pyx
# 执行以下命令
# cd g6k-fpylll
# python setup.py build_ext -j32
# python setup.py install
# cd ..

cd kernel
make clean
cd ..
rm -rf ./build
python setup.py clean
python setup.py build_ext -j 16 --inplace
