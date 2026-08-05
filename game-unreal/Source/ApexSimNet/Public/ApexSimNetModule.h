#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

APEXSIMNET_API DECLARE_LOG_CATEGORY_EXTERN(LogApexSimNet, Log, All);

class FApexSimNetModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
