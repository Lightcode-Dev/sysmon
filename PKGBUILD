# Maintainer: Nathan Burke <dev@bigland.cc>
pkgname=sysmon
pkgver=1.0.0
pkgrel=1
pkgdesc='Live terminal system monitor with an interactive process table'
arch=('x86_64')
url='https://github.com/Lightcode-Dev/sysmon'
license=('MIT')   # TODO: change to the license you choose, and add a LICENSE file to the repo
depends=('glibc')
makedepends=('gcc' 'make')
source=("$pkgname-$pkgver.tar.gz::$url/archive/refs/tags/v$pkgver.tar.gz")
sha256sums=('SKIP')   # run `updpkgsums` once the v$pkgver tag is pushed

build() {
  cd "$pkgname-$pkgver"
  # makepkg exports CFLAGS and LDFLAGS from /etc/makepkg.conf; the Makefile honours them
  make CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS"
}

package() {
  cd "$pkgname-$pkgver"
  make DESTDIR="$pkgdir" PREFIX=/usr install
  # a LICENSE file does not exist yet; once it does, uncomment:
  # install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
