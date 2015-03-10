#
# Include the TEA standard macro set
#

builtin(include,tclconfig/tcl.m4)

#
# Add here whatever m4 macros you want to define for your package
#

# 
# Create sysconfig.tcl from tclConfig.sh and other things
#
AC_DEFUN([CASSTCL_MAKE_PGTCL], [

AC_REQUIRE([TEA_PATH_TCLCONFIG])
AC_REQUIRE([TEA_LOAD_TCLCONFIG])

# Handle the --with-pgsql configure option.
AC_ARG_WITH([pgsql],
	[  --with-pgsql[=PATH]       Build with pgsql/pgtcl library support],
[
AC_MSG_CHECKING([location of pgsql and pgtcl])
if test "x$withval" = "x" -o "$withval" = "yes"; then
  pg_prefixes="/usr/local /usr/local/pgsql /usr"
else
  pg_prefixes=$withval
fi

# Look for pgsql and pgtcl
for prefix in $pg_prefixes
do
  # look for pgsql frontend header
  if test -f $prefix/include/libpq-fe.h; then
sysconfig_tcl_content="$sysconfig_tcl_content
set sysconfig(pqprefix) $prefix"
  else
    continue
  fi

  # there may be multiple installed versions of pgtcl so sort with the highest version first.
  pg_libdirs=`find $prefix/lib -maxdepth 1 -name "pgtcl*" -type d | sort -rn`

  if test -z "$pg_libdirs"; then
     continue
  fi

  # look for pgtcl-ng before pgtcl
  pgtclver=""
  for dir in $pg_libdirs
  do
    if test -f $dir/pkgIndex.tcl; then
      #pgtclver=`basename $dir | sed s/^pgtcl//`
      pgtclver=`grep "package ifneeded Pgtcl" $dir/pkgIndex.tcl| cut -d' ' -f4`
      sysconfig_tcl_content="$sysconfig_tcl_content
      set sysconfig(pgtclver) $pgtclver
      set sysconfig(pgtclprefix) $dir"
      break
    fi
  done

  # look for pgtcl
  if test -z "$pgtclver"; then
    for dir in $pg_libdirs
    do
      if test -f $dir/pgtcl.tcl; then
        pgtclver=`basename $dir | sed s/^pgtcl//`
        sysconfig_tcl_content="$sysconfig_tcl_content
        set sysconfig(pgtclver) $pgtclver
        set sysconfig(pgtclprefix) $prefix"
	break
      fi
    done
  fi

  pg_prefixes=$dir
  break
done

if test -z "$pgtclver"; then
  AC_MSG_ERROR([pgsql and/or pgtcl not found under $pg_prefixes])
fi

AC_MSG_RESULT([found under $pg_prefixes])

])

])

