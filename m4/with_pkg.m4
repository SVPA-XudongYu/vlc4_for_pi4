dnl with_pkg.m4 - Macros to ease the usage of pkg-config.    -*- Autoconf -*-
dnl
dnl Copyright © 2008 Luca Barbato <lu_zero@gentoo.org>,
dnl                  Diego Pettenò <flameeyes@gentoo.org>
dnl                  Jean-Baptiste Kempf
dnl
dnl This program is free software; you can redistribute it and/or modify
dnl it under the terms of the GNU General Public License as published by
dnl the Free Software Foundation; either version 2 of the License, or
dnl (at your option) any later version.
dnl
dnl This program is distributed in the hope that it will be useful, but
dnl WITHOUT ANY WARRANTY; without even the implied warranty of
dnl MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
dnl General Public License for more details.
dnl
dnl You should have received a copy of the GNU General Public License
dnl along with this program; if not, write to the Free Software
dnl Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
dnl
dnl As a special exception to the GNU General Public License, if you
dnl distribute this file as part of a program that contains a
dnl configuration script generated by Autoconf, you may include it under
dnl the same distribution terms that you use for the rest of that program.

dnl PKG_WITH_MODULES(VARIABLE-PREFIX, MODULES,
dnl                  [ACTION-IF-FOUND],[ACTION-IF-NOT-FOUND],
dnl                  [DESCRIPTION], [DEFAULT])
dnl
dnl Prepare a --with-variable-prefix triggered check for module,
dnl disable by default.
dnl

AC_DEFUN([PKG_WITH_MODULES],
[
AC_REQUIRE([PKG_PROG_PKG_CONFIG])
m4_pushdef([with_arg], m4_tolower([$1]))

m4_pushdef([description],
           [m4_default([$5], [build with ]with_arg[ support enabled])])

m4_pushdef([def_arg], [m4_default([$6], [auto])])
m4_pushdef([def_action_if_found], [AS_TR_SH([enable_]with_arg)=yes])
m4_pushdef([def_action_if_not_found], [AS_TR_SH([enable_]with_arg)=no])

m4_case(def_arg,
            [yes],[m4_pushdef([with_without], [--disable-]with_arg)],
            [m4_pushdef([with_without],[--enable-]with_arg)])

AC_ARG_ENABLE(with_arg,
     AS_HELP_STRING(with_without, description[ @<:@default=]def_arg[@:>@]),,
    [AS_TR_SH([enable_]with_arg)=def_arg])

AS_CASE([$AS_TR_SH([enable_]with_arg)],
            [yes],[PKG_CHECK_MODULES([$1],[$2],$3,$4)],
            [auto],[PKG_CHECK_MODULES([$1],[$2],
                                        [m4_n([def_action_if_found]) $3],
                                        [m4_n([def_action_if_not_found]) $4])])

m4_popdef([with_arg])
m4_popdef([description])
m4_popdef([def_arg])

]) dnl PKG_WITH_MODULES

dnl PKG_HAVE_WITH_MODULES(VARIABLE-PREFIX, MODULES,
dnl                       [DESCRIPTION], [DEFAULT])
dnl

AC_DEFUN([PKG_HAVE_WITH_MODULES],
[
PKG_WITH_MODULES([$1],[$2],,,[$3],[$4])

AM_CONDITIONAL([HAVE_][$1],
               [test "$AS_TR_SH([with_]m4_tolower([$1]))" = "yes"])
])

dnl PKG_ENABLE_MODULES_VLC(VARIABLE-PREFIX,
dnl                         VLC_MODULE_NAME  dnl (if empty, same as VARIABLE-PREFIX)
dnl                         PKG MODULES,
dnl                         [DESCRIPTION], [DEFAULT],
dnl                         [EXTRA_CFLAGS], [EXTRA_LIBS])
AC_DEFUN([PKG_ENABLE_MODULES_VLC],
[
m4_pushdef([module_name], m4_default(m4_tolower([$2]),m4_tolower([$1])))
m4_pushdef([enable_arg], m4_tolower([$1]))

PKG_WITH_MODULES([$1],[$3],
    VLC_ADD_PLUGIN(module_name)
    VLC_ADD_CFLAGS(module_name,[$$1_CFLAGS] [$6])
    VLC_ADD_LIBS(module_name,[$$1_LIBS] [$7]),
    AS_IF([test x"$AS_TR_SH([enable_]enable_arg)" = "xyes"],
        [AC_MSG_ERROR(Library [$3] needed for [m4_tolower([$1])] was not found)],
        [AC_MSG_WARN(Library [$3] needed for [m4_tolower([$1])] was not found)]
         ),
    [$4],[$5])

AM_CONDITIONAL([HAVE_][$1],
               [test "$AS_TR_SH([with_]enable_arg)" = "yes"])

m4_popdef([module_name])
m4_popdef([enable_arg])

])

