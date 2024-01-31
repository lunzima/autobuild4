#!/bin/bash
##15-python.sh: Builds Python PEP517 stuff
##@copyright GPL-2.0+

# PEP517 is only supported in Python 3

build_pep517_probe(){
	[ -f "$SRCDIR"/pyproject.toml ]
}

build_pep517_configure() {
	BUILD_START
	if bool "$NOPYTHON3"; then
		abdie "PEP517 is only supported in Python 3. Specifying NOPYTHON3 is contradictory!"
	fi
	if ! bool "$NOPYTHON2"; then
		abwarn "PEP517 is only supported in Python 3. Please specify NOPYTHON2=1 to suppress this warning."
	fi

	abinfo "Detecting PEP517 build backend..."
	local __PEP517_BUILD_BACKEND="$(python3 -c "import os; import tomli; build_sys = tomli.load(open(os.path.join('${SRCDIR}','pyproject.toml'), 'rb'))['build-system']; print(build_sys['build-backend']);")"
	if [[ "$__PEP517_BUILD_BACKEND" == "setuptools.build_meta" ]]; then
		abinfo "Detected setuptools backend, that needs a workaround."
		mkdir "${SRCDIR}/.tempsrc"
		mv * "${SRCDIR}/.tempsrc"
		mv "${SRCDIR}/.tempsrc" "${SRCDIR}/${PKGNAME}_src"
		mv "${SRCDIR}/${PKGNAME}_src/"abqaerr.log "${SRCDIR}"
		mv "${SRCDIR}/${PKGNAME}_src/"acbs-build*.log "${SRCDIR}"
		mv "${SRCDIR}/${PKGNAME}_src/"autobuild "${SRCDIR}"
	fi
}

build_pep517_build() {
	BUILD_READY
	abinfo "Building Python (PEP517) package ..."
	if [[ -d "${SRCDIR}"/"${PKGNAME}"_src/ ]]; then
		python3 \
			-m build \
			--no-isolation \
			--wheel \
			--outdir "${SRCDIR}/dist" \
			"${SRCDIR}/${PKGNAME}_src" \
		|| abdie "Failed to build Python (PEP517) package: $?."
	else
		python3 \
			-m build \
			--no-isolation \
			--wheel \
		|| abdie "Failed to build Python (PEP517) package: $?."
	fi
}

build_pep517_install() {
	abinfo "Installing Python (PEP517) package ..."
	python3 \
		-m installer \
		--destdir="$PKGDIR" \
		"$SRCDIR"/dist/*.whl
	BUILD_FINAL
}

ab_register_template pep517 -- python3 pip3
