@ stub SLpAuthenticateGenuineTicketResponse
@ stub SLpBeginGenuineTicketTransaction
@ stub SLpCheckProductKey
@ stub SLpGetGenuineBlob
@ stub SLpGetGenuineLocal
@ stub SLpGetLicenseAcquisitionInfo
@ stub SLpGetMachineUGUID
@ stub SLpVLActivateProduct
@ stdcall SLClose(ptr) sppc.SLClose
@ stdcall SLConsumeRight(ptr ptr ptr wstr ptr) sppc.SLConsumeRight
@ stub SLConsumeWindowsRight
@ stdcall SLDepositOfflineConfirmationId(ptr ptr wstr wstr) sppc.SLDepositOfflineConfirmationId
@ stdcall SLFireEvent(ptr wstr ptr) sppc.SLFireEvent
@ stdcall SLGenerateOfflineInstallationId(ptr ptr ptr) sppc.SLGenerateOfflineInstallationId
@ stdcall SLGetApplicationInformation(ptr ptr wstr ptr ptr ptr) sppc.SLGetApplicationInformation
@ stdcall SLGetAuthenticationResult(ptr ptr ptr) sppc.SLGetAuthenticationResult
@ stdcall SLGetInstalledProductKeyIds(ptr ptr ptr ptr) sppc.SLGetInstalledProductKeyIds
@ stub SLGetInstalledSAMLicenseApplications
@ stub SLGetLicense
@ stdcall SLGetLicenseFileId(ptr long ptr ptr) sppc.SLGetLicenseFileId
@ stdcall SLGetLicenseInformation(ptr ptr wstr ptr ptr ptr) sppc.SLGetLicenseInformation
@ stdcall SLGetLicensingStatusInformation(ptr ptr ptr wstr ptr ptr) sppc.SLGetLicensingStatusInformation
@ stdcall SLGetPKeyId(ptr wstr wstr long ptr ptr) sppc.SLGetPKeyId
@ stdcall SLGetPKeyInformation(ptr ptr wstr ptr ptr ptr) sppc.SLGetPKeyInformation
@ stdcall SLGetPolicyInformation(ptr wstr ptr ptr ptr) sppc.SLGetPolicyInformation
@ stdcall SLGetPolicyInformationDWORD(ptr wstr ptr) sppc.SLGetPolicyInformationDWORD
@ stdcall SLGetProductSkuInformation(ptr ptr wstr ptr ptr ptr) sppc.SLGetProductSkuInformation
@ stub SLGetSAMLicense
@ stdcall SLGetSLIDList(ptr long ptr long ptr ptr) sppc.SLGetSLIDList
@ stdcall SLGetServiceInformation(ptr wstr ptr ptr ptr) sppc.SLGetServiceInformation
@ stdcall SLGetWindowsInformation(wstr ptr ptr ptr)
@ stdcall SLGetWindowsInformationDWORD(wstr ptr)
@ stdcall SLInstallLicense(ptr long ptr ptr) sppc.SLInstallLicense
@ stdcall SLInstallProofOfPurchase(ptr wstr wstr long ptr ptr) sppc.SLInstallProofOfPurchase
@ stub SLInstallSAMLicense
@ stdcall SLOpen(ptr) sppc.SLOpen
@ stdcall SLReArm(ptr ptr ptr long) sppc.SLReArm
@ stub SLReArmWindows
@ stdcall SLRegisterEvent(ptr wstr ptr ptr) sppc.SLRegisterEvent
@ stub SLRegisterWindowsEvent
@ stdcall SLUninstallLicense(ptr ptr) sppc.SLUninstallLicense
@ stdcall SLUninstallProofOfPurchase(ptr ptr) sppc.SLUninstallProofOfPurchase
@ stub SLUninstallSAMLicense
@ stdcall SLUnregisterEvent(ptr wstr ptr ptr) sppc.SLUnregisterEvent
@ stub SLUnregisterWindowsEvent
