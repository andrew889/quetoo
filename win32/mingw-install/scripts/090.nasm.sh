PKGNAME="nasm"
PKGVER="2.10.05"

SOURCE=http://www.nasm.us/pub/${PKGNAME}/releasebuilds/${PKGVER}/${PKGNAME}-${PKGVER}.tar.gz

pushd ../source
wget -c $SOURCE
popd 

tar xzf ../source/${PKGNAME}-${PKGVER}.tar.gz

cd ${PKGNAME}-${PKGVER}

./configure --prefix=/mingw/local
make -j 4
make install