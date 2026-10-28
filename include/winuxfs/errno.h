#ifndef WINUXFS_ERRNO_H
#define WINUXFS_ERRNO_H

/* Linux errno values: the error space of the backend ABI.
 *
 * A failing operation returns -WFS_Exxx. Backends must use these constants
 * (or the same literal numbers) and NOT the C runtime's <errno.h>: the values
 * differ between platforms (mingw: ENOSYS 40, ENOTEMPTY 41, ENAMETOOLONG 38,
 * ELOOP 114), and the host maps the numbers as Linux errnos. A number
 * outside 1..133 is treated as EIO.
 *
 * Names carry the WFS_ prefix so they never clash with <errno.h>.
 * Generic (x86/x86-64/arm64) Linux numbering.
 */

#define WFS_EPERM            1
#define WFS_ENOENT           2
#define WFS_ESRCH            3
#define WFS_EINTR            4
#define WFS_EIO              5
#define WFS_ENXIO            6
#define WFS_E2BIG            7
#define WFS_ENOEXEC          8
#define WFS_EBADF            9
#define WFS_ECHILD           10
#define WFS_EAGAIN           11
#define WFS_ENOMEM           12
#define WFS_EACCES           13
#define WFS_EFAULT           14
#define WFS_ENOTBLK          15
#define WFS_EBUSY            16
#define WFS_EEXIST           17
#define WFS_EXDEV            18
#define WFS_ENODEV           19
#define WFS_ENOTDIR          20
#define WFS_EISDIR           21
#define WFS_EINVAL           22
#define WFS_ENFILE           23
#define WFS_EMFILE           24
#define WFS_ENOTTY           25
#define WFS_ETXTBSY          26
#define WFS_EFBIG            27
#define WFS_ENOSPC           28
#define WFS_ESPIPE           29
#define WFS_EROFS            30
#define WFS_EMLINK           31
#define WFS_EPIPE            32
#define WFS_EDOM             33
#define WFS_ERANGE           34
#define WFS_EDEADLK          35
#define WFS_ENAMETOOLONG     36
#define WFS_ENOLCK           37
#define WFS_ENOSYS           38
#define WFS_ENOTEMPTY        39
#define WFS_ELOOP            40
#define WFS_ENOMSG           42
#define WFS_EIDRM            43
#define WFS_ECHRNG           44
#define WFS_EL2NSYNC         45
#define WFS_EL3HLT           46
#define WFS_EL3RST           47
#define WFS_ELNRNG           48
#define WFS_EUNATCH          49
#define WFS_ENOCSI           50
#define WFS_EL2HLT           51
#define WFS_EBADE            52
#define WFS_EBADR            53
#define WFS_EXFULL           54
#define WFS_ENOANO           55
#define WFS_EBADRQC          56
#define WFS_EBADSLT          57
#define WFS_EBFONT           59
#define WFS_ENOSTR           60
#define WFS_ENODATA          61
#define WFS_ETIME            62
#define WFS_ENOSR            63
#define WFS_ENONET           64
#define WFS_ENOPKG           65
#define WFS_EREMOTE          66
#define WFS_ENOLINK          67
#define WFS_EADV             68
#define WFS_ESRMNT           69
#define WFS_ECOMM            70
#define WFS_EPROTO           71
#define WFS_EMULTIHOP        72
#define WFS_EDOTDOT          73
#define WFS_EBADMSG          74
#define WFS_EOVERFLOW        75
#define WFS_ENOTUNIQ         76
#define WFS_EBADFD           77
#define WFS_EREMCHG          78
#define WFS_ELIBACC          79
#define WFS_ELIBBAD          80
#define WFS_ELIBSCN          81
#define WFS_ELIBMAX          82
#define WFS_ELIBEXEC         83
#define WFS_EILSEQ           84
#define WFS_ERESTART         85
#define WFS_ESTRPIPE         86
#define WFS_EUSERS           87
#define WFS_ENOTSOCK         88
#define WFS_EDESTADDRREQ     89
#define WFS_EMSGSIZE         90
#define WFS_EPROTOTYPE       91
#define WFS_ENOPROTOOPT      92
#define WFS_EPROTONOSUPPORT  93
#define WFS_ESOCKTNOSUPPORT  94
#define WFS_EOPNOTSUPP       95
#define WFS_EPFNOSUPPORT     96
#define WFS_EAFNOSUPPORT     97
#define WFS_EADDRINUSE       98
#define WFS_EADDRNOTAVAIL    99
#define WFS_ENETDOWN         100
#define WFS_ENETUNREACH      101
#define WFS_ENETRESET        102
#define WFS_ECONNABORTED     103
#define WFS_ECONNRESET       104
#define WFS_ENOBUFS          105
#define WFS_EISCONN          106
#define WFS_ENOTCONN         107
#define WFS_ESHUTDOWN        108
#define WFS_ETOOMANYREFS     109
#define WFS_ETIMEDOUT        110
#define WFS_ECONNREFUSED     111
#define WFS_EHOSTDOWN        112
#define WFS_EHOSTUNREACH     113
#define WFS_EALREADY         114
#define WFS_EINPROGRESS      115
#define WFS_ESTALE           116
#define WFS_EUCLEAN          117
#define WFS_ENOTNAM          118
#define WFS_ENAVAIL          119
#define WFS_EISNAM           120
#define WFS_EREMOTEIO        121
#define WFS_EDQUOT           122
#define WFS_ENOMEDIUM        123
#define WFS_EMEDIUMTYPE      124
#define WFS_ECANCELED        125
#define WFS_ENOKEY           126
#define WFS_EKEYEXPIRED      127
#define WFS_EKEYREVOKED      128
#define WFS_EKEYREJECTED     129
#define WFS_EOWNERDEAD       130
#define WFS_ENOTRECOVERABLE  131
#define WFS_ERFKILL          132
#define WFS_EHWPOISON        133

/* aliases, as on Linux */
#define WFS_EWOULDBLOCK      WFS_EAGAIN
#define WFS_EDEADLOCK        WFS_EDEADLK

#endif /* WINUXFS_ERRNO_H */
