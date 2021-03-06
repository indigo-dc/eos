.. highlight:: rst

.. index::
   single: Permission System

Permission System
=================

Overview
--------

The EOS permission system is based on a combination of **ACLs**  and **POSIX** permissions.

There are two major differences to traditional storage systems:

1   Files don't carry their permissions (only the ownership for quota accounting). 
They inherit the permissions from the parent directory automatically!
2   Permissions are only checked in the direct parent, EOS is not walking through the complete directory hierarchy.

UNIX Permissions

EOS allows to set user, group and other permissions for read write and browsing defined 
by ``'r'(4), 'w'(2), 'x'(1)`` e.g. ``777 ='rwx'``.

Unlike in POSIX the S_ISGID (2---) indicates that any new directory created should inherited automatically all the 
extended attributes defined at creation time from the parent directory.

If the parent attributes are changed after creation, they are not automatically 
applied to its children. The inheritance bit is always added to any *chmod* automatically. >

All modes for *chmod* have to be given in octal format. For details see **eos chmod --help**.

The S_ISVTX bit (1---) is displayed whenever a directory has any extended attribute defined.

ACLs
----
ACLs are defined only on the directory level via the extended attribute

.. code-block:: bash

   sys.acl=<acllist>

   user.acl=<acllist>

The system attribute can only be defined by SUDO members. 
If  sys.acl is defined user.acl is ignored. 
The user attribute can be defined by the **owner** or SUDO members.

<acllist> is defined as a comma separated list of rules:

.. code-block:: bash
   
   <acllist> = <rule1>,<rule2>...<ruleN>

A rule is defined in the following way:

.. code-block:: bash

   <rule> = u:<uid|username>|g:<gid|groupname>|egroup:<name>|z::{rwxomqci(!d)(+d)(!u)(+u)}

A rule has three colon separated fields. It starts with the type of rule: 
User (u), Group (g), eGroup (egroup) or all (z). The second field specifies the name or 
the unix ID of user/group rules and the eGroup name for eGroups  
The last field contains the rule definition. 

The following tags compose a rule:

.. epigraph::

   === =========================================================================
   tag definition
   === =========================================================================
   r   grant read permission
   w   grant write permissions
   x   grant browsing permission
   m   grant change mode permission
   !m  forbid change mode operations
   !d  forbid deletion of files and directories
   +d  overwrite a '!d' rule and allow deletion of files and directories
   !u  forbid update of files
   +u  overwrite a '!u' rule and allow updates for files 
   q   grant 'set quota' permissions on a quota node
   c   grant 'change owner' permission on directory children
   i   set the immutable flag    
   =============================================================================

A complex example is shown here:

.. code-block:: bash

   sys.acl="u:300:rw!u,g:z2:rwo,egroup:eos-dev:rwx,u:dummy:rwm!d,u:adm:rwxmqc"

   # user id 300 can read + write, but not update
   #
   # group z2 can read + write-once (create new files but can't delete)
   #
   # members of egroup 'eos-dev' can read & write & browse
   #
   # user name dummy can read + write into directory and modify the permissions 
   # (chmod), but cannot delete directories inside which are not owned by him.
   #
   # user name adm can read,write,browse, change-mod, set quota on that 
   # directory and change the ownership of directory children

.. note::

   Write-once and '!d' or '!u' rules remove permissions which can only be regained 
   by a second rule adding the '+u' or '+d' flag e.g. if the matching user ACL 
   forbids deletion it is not granted if a group rule does not forbid deletion!

It is possible to write rules, which apply to everyone:

.. code-block:: bash

   sys.acl="z:i"
 
   # this directory is immutable for everybody


Finally an ACL is set e.g.:

.. code-block:: bash

   eos attr set sys.acl=... /eos/mypath


The ACLs can be listed by:

.. code-block:: bash

   eos attr ls /eos/mypath

Validity of Permissions
----------------------------

File Access
+++++++++++

A user can read a file if the parent directory grants 'r' access via the ACL 
rules to the user's uid/gid pair. If the ACL does not grant the access, 
UNIX permissions are evaluated for a matching 'r' permission bit.

A user can create a file if the parent directory grants 'w' access via the ACL 
rules to the user's uid/gid pair. A user cannot overwrite a file if the ACL 
grants 'wo' permission. If the ACL does not grant the access, UNIX permissions 
are evaluated for a matching 'w' permission bit.

.. note::

   The root role (uid=0 gid=0) can always read and write any file. 
   The daemon role (uid=2) can always read any file.

File Deletion
+++++++++++++

A file can be deleted if the parent directory grants 'w' access via the ACL 
rules to the user's uid/gid pair. A user cannot delete a file, 
if the ACL grants 'wo' or '!d' permission. 

.. note:: 

   The root role (uid=0 gid=0) can 
   always delete any file. 

File Permission Modification
++++++++++++++++++++++++++++

File permissions cannot be changed, they are automatically inherited from the
parent directory.

File Ownership
++++++++++++++

A user can change the ownership of a file if he/she is member of the SUDO group. 
The root, admin user and admin group role can always change the ownership of a 
file. See **eos chown --help**  for details.

Directory Access
++++++++++++++++

A user can create a directory if he has the UNIX 'wx' permission or the ACL 
rules grant the 'w' or 'wo' permission. T
he root role can always create any directory.

A user can list a directory if the UNIX permissions grant 'rx' or the ACL 
grants 'x' rights. 

.. note::
   
   The root, admin user and admin group role can always 
   browse directories.

Directory Deletion
++++++++++++++++++

A user can delete a directory if he/she is the owner of the directory. 
A user can delete a directory if he/she is not the owner of that directory 
in case 'UNIX 'w'permission are granted and '!d' is not defined by a matching 
ACL rule. 

.. note::

   The root role can always delete any directory.

.. warning::

   Deletion only works, if directories are empty!

Directory Permission Modification
+++++++++++++++++++++++++++++++++

A user can modify the UNIX permissions if he/she is the owner of the file 
and/or the parent directory ACL rules grant the 'm' rights. 

.. note::

   The root, admin 
   user and admin group role can always modify the UNIX permissions.

Directory ACL Modification
++++++++++++++++++++++++++

A user can modify the system ACL, if he/she is member of the SUDO group. 
A user can modify a user ACL, if he/she is the owner of the directory or 
member of the SUDO group.

Directory Ownership
+++++++++++++++++++

The root, admin user and admin group role can always change the directory 
owner and group. 

.. warning:: 

   Non priviledged users can not change the ownership.

Quota Permission
++++++++++++++++

A user can do 'quota set' if he is a sudoer, has the 'q' ACL permission set on 
the quota node or on the proc directory ``/eos/<instance>/proc``.

How to setup a shared scratch directory
+++++++++++++++++++++++++++++++++++++++

If a directory is group writable one should add an ACL entry for this group 
to forbid the deletion of files and directories to non-owners and allow 
deletion to a dedicated account:

E.g. to define a scratch directory for group 'vl' and the deletion 
user 'prod' execute:

.. code-block:: bash

   eos attr set sys.acl=g:vl:!d,u:prod:+d /eos/dev/scratchdisk

How to setup a shared group directory
+++++++++++++++++++++++++++++++++++++

A directory shared by a <group> with variable members should be setup like this:

.. code-block:: bash

   chmod 550 <groupdir>
   eos attr set sys.acl="egroup:<group>:rw!m"

Sticky Ownership
+++++++++++++++++++++++++++++++++++++++

The ACL tag sys.owner.auth allows to tag clients acting as the owner of a directory. The value normally is composed by the authentication method and the user name or can be a wildcard.
If a wild card is specified, everybody resulting in having write permission can use the sticky ownership and write into a directory on behalf of the owner e.g. the file is owned by the directory
owner and not by the authenticated client and quota is booked on the directory owner.

.. code-block:: bash

   eos attr set sys.owner.auth="krb5:prod"
   eos attr set sys.owner.auth="*"

Permission Masks
++++++++++++++++

A permission mask which is applied on all chmod requests for directories can be defined via:

.. code-block:: bash

   sys.mask=<octal-mask>

Example:

.. code-block:: bash

   eos attr set sys.mask="770"
   eos chmod 777 <dir>
   success: mode of file/directory <dir> is now '770'

When the mask attribute is set the !m flag is automatically disabled even if it is given in the ACL.

ACL CLI
+++++++

To provide atomic add,remove and replacement of permissions one can take advantage of the ``eos acl`` command instead of modifying directly the `sys.acl` attribute:

.. code-block:: bash

   Usage: eos acl [-l|--list] [-R|--recursive][--sys|--user] <rule> <path>

       --help           Print help
   -R, --recursive      Apply on directories recursively
   -l, --lists          List ACL rules
       --user           Set user.acl rules on directory
       --sys            Set sys.acl rules on directory
   <rule> is created based on chmod rules. 
   Every rule begins with [u|g|egroup] followed with : and identifier.

   Afterwards can be:
   = for setting new permission .
   : for modification of existing permission.
  
   This is followed by the rule definition.
   Every ACL flag can be added with + or removed with -, or in case
   of setting new ACL permission just enter the ACL flag.

