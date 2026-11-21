@ stub SLCallServer
@ stub SLpAuthenticateGenuineTicketResponse
@ stub SLpBeginGenuineTicketTransaction
@ stub SLpClearActivationInProgress
@ stub SLpDepositDownlevelGenuineTicket
@ stub SLpDepositTokenActivationResponse
@ stub SLpGenerateTokenActivationChallenge
@ stub SLpGetGenuineBlob
@ stub SLpGetGenuineLocal
@ stub SLpGetLicenseAcquisitionInfo
@ stub SLpGetMSPidInformation
@ stub SLpGetMachineUGUID
@ stub SLpGetTokenActivationGrantInfo
@ stub SLpIAActivateProduct
@ stub SLpIsCurrentInstalledProductKeyDefaultKey
@ stub SLpProcessVMPipeMessage
@ stub SLpSetActivationInProgress
@ stub SLpTriggerServiceWorker
@ stub SLpVLActivateProduct
@ stdcall SLClose(ptr)
@ stdcall SLConsumeRight(ptr ptr ptr wstr ptr)
@ stub SLDepositMigrationBlob
@ stub SLDepositOfflineConfirmationId
@ stub SLDepositOfflineConfirmationIdEx
@ stub SLDepositStoreToken
@ stub SLFireEvent
@ stub SLGatherMigrationBlob
@ stub SLGatherMigrationBlobEx
@ stub SLGenerateOfflineInstallationId
@ stub SLGenerateOfflineInstallationIdEx
@ stub SLGetActiveLicenseInfo
@ stub SLGetApplicationInformation
@ stdcall SLGetApplicationPolicy(ptr wstr ptr ptr ptr)
@ stub SLGetAuthenticationResult
@ stub SLGetEncryptedPIDEx
@ stub SLGetGenuineInformation
@ stub SLGetInstalledProductKeyIds
@ stub SLGetLicense
@ stdcall SLGetLicenseFileId(ptr long ptr ptr)
@ stub SLGetLicenseInformation
@ stdcall SLGetLicensingStatusInformation(ptr ptr ptr wstr ptr ptr)
@ stub SLGetPKeyId
@ stub SLGetPKeyInformation
@ stdcall SLGetPolicyInformation(ptr wstr ptr ptr ptr)
@ stdcall SLGetPolicyInformationDWORD(ptr wstr ptr)
@ stdcall SLGetProductSkuInformation(ptr ptr wstr ptr ptr ptr)
@ stdcall SLGetSLIDList(ptr long ptr long ptr ptr)
@ stub SLGetServiceInformation
@ stdcall SLInstallLicense(ptr long ptr ptr)
@ stdcall SLInstallProofOfPurchase(ptr wstr wstr long ptr ptr)
@ stub SLInstallProofOfPurchaseEx
@ stub SLIsGenuineLocalEx
@ stdcall SLLoadApplicationPolicies(ptr ptr long ptr)
@ stdcall SLOpen(ptr)
@ stdcall SLPersistApplicationPolicies(ptr ptr long)
@ stub SLPersistRTSPayloadOverride
@ stub SLReArm
@ stdcall SLRegisterEvent(ptr wstr ptr ptr)
@ stdcall SLRegisterPlugin(ptr ptr ptr ptr)
@ stdcall SLSetAuthenticationData(ptr long ptr)
@ stub SLSetCurrentProductKey
@ stub SLSetGenuineInformation
@ stdcall SLUninstallLicense(ptr ptr)
@ stdcall SLUninstallProofOfPurchase(ptr ptr)
@ stdcall SLUnloadApplicationPolicies(ptr long)
@ stdcall SLUnregisterEvent(ptr wstr ptr ptr)
@ stdcall SLUnregisterPlugin(ptr ptr ptr ptr)
