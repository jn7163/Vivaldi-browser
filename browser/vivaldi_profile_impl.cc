// Copyright (c) 2019 Vivaldi Technologies AS. All rights reserved.

#include "browser/vivaldi_profile_impl.h"

#include "app/vivaldi_apptools.h"
#include "base/command_line.h"
#include "calendar/calendar_model_loaded_observer.h"
#include "calendar/calendar_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "components/translate/core/browser/translate_language_list.h"
#include "components/translate/core/browser/translate_pref_names.h"
#include "contact/contact_model_loaded_observer.h"
#include "contact/contact_service_factory.h"
#include "content/public/common/content_switches.h"
#include "extensions/buildflags/buildflags.h"
#include "notes/notes_factory.h"
#include "notes/notes_model.h"
#include "notes/notes_model_loaded_observer.h"
#include "ui/lazy_load_service_factory.h"
#include "vivaldi/prefs/vivaldi_gen_pref_enums.h"
#include "vivaldi/prefs/vivaldi_gen_prefs.h"

#if BUILDFLAG(ENABLE_EXTENSIONS)
#include "extensions/api/vivaldi_utilities/vivaldi_utilities_api.h"
#endif


namespace vivaldi {
void VivaldiInitProfile(Profile* profile) {
  PrefService* pref_service = profile->GetPrefs();
  pref_service->SetBoolean(prefs::kOfferTranslateEnabled, false);
#if !defined(OS_ANDROID)
  vivaldi::Notes_Model* notes_model =
      vivaldi::NotesModelFactory::GetForBrowserContext(profile);
  notes_model->AddObserver(new vivaldi::NotesModelLoadedObserver(profile));

  calendar::CalendarService* calendar_service =
      calendar::CalendarServiceFactory::GetForProfile(profile);
  calendar_service->AddObserver(new calendar::CalendarModelLoadedObserver());

  contact::ContactService* contact_service =
      contact::ContactServiceFactory::GetForProfile(profile);
  contact_service->AddObserver(new contact::ContactModelLoadedObserver());

  extensions::VivaldiUtilitiesAPI::GetFactoryInstance()
      ->Get(profile)
      ->PostProfileSetup();

  if (!vivaldi::IsVivaldiRunning())
    return;

  vivaldi::LazyLoadServiceFactory::GetForProfile(profile);

  if (pref_service->GetBoolean(vivaldiprefs::kWebpagesSmoothScrollingEnabled) ==
      false) {
    vivaldi::CommandLineAppendSwitchNoDup(
        base::CommandLine::ForCurrentProcess(),
        switches::kDisableSmoothScrolling);
  }
  int spatnav =
      pref_service->GetInteger(vivaldiprefs::kWebpagesSpatialNavigationMethod);
  if (spatnav ==
      static_cast<int>(
          vivaldiprefs::WebpagesSpatialNavigationMethodValues::BLINK)) {
    vivaldi::CommandLineAppendSwitchNoDup(
        base::CommandLine::ForCurrentProcess(),
        switches::kEnableSpatialNavigation);
  }
#endif
}

}  // namespace vivaldi
