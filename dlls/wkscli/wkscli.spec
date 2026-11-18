@ stub NetAddAlternateComputerName
@ stub NetEnumerateComputerNames
@ stdcall NetGetJoinInformation( wstr ptr ptr )
@ stub NetGetJoinableOUs
@ stdcall NetJoinDomain(wstr wstr wstr wstr wstr long) wexjoin.NetJoinDomain
@ stub NetRemoveAlternateComputerName
@ stdcall NetRenameMachineInDomain(wstr wstr wstr wstr long) wexjoin.NetRenameMachineInDomain
@ stub NetSetPrimaryComputerName
@ stdcall NetUnjoinDomain(wstr wstr wstr long) wexjoin.NetUnjoinDomain
@ stdcall NetUseAdd( str long ptr ptr )
@ stdcall NetUseDel( str str long )
@ stdcall NetUseEnum( str long ptr long ptr ptr ptr )
@ stdcall NetUseGetInfo( str str long ptr )
@ stdcall NetValidateName(wstr wstr wstr wstr long) wexjoin.NetValidateName
@ stub NetWkstaGetInfo( str long ptr )
@ stub NetWkstaSetInfo
@ stub NetWkstaStatisticsGet
@ stub NetWkstaTransportAdd
@ stub NetWkstaTransportDel
@ stdcall NetWkstaTransportEnum( str long ptr long ptr ptr ptr )
@ stub NetWkstaUserEnum( str long ptr long ptr ptr ptr )
@ stub NetWkstaUserGetInfo( str long ptr )
@ stub NetWkstaUserSetInfo
