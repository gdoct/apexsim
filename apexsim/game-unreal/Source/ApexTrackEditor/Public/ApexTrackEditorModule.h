#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

APEXTRACKEDITOR_API DECLARE_LOG_CATEGORY_EXTERN(LogApexTrackImport, Log, All);

/**
 * Editor-only module that turns baked track exports into levels.
 *
 * Nothing here ships in a game build: the whole pipeline runs offline, in
 * the editor or from `ApexSimEditor-Cmd -run=ApexTrackImport`.
 */
class FApexTrackEditorModule : public IModuleInterface
{
};
