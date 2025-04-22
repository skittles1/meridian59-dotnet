#include "stdafx.h"

// percentage of base volume for self-origin sounds
const float soundVolumeSelf = 0.7f;

// percentage of base volume from behind
const float soundVolumeRear = 0.4f;

// degrees, angle where volume starts dropping
const float soundReductionAngle = 30.0f;

// percentage of base volume at right angle (to adjust for spike in volume)
const float rightAngleAdjustment = 0.8f;

// width of right angle cone across which adjustment occurs
const float rightAngleCone = 180.0f;

namespace Meridian59 { namespace Ogre 
{
   float GetAttenuatedVolume(
      const vec3df& listenerPos,
      const vec3df& listenerLook,
      bool isSelfOrigin,
      const vec3df& soundPos,
      float baseVolume)
   {
      if (isSelfOrigin)
         return baseVolume * soundVolumeSelf;
   
      vec3df toSound = soundPos - listenerPos;
      ik_f64 len = toSound.getLength();
   
      if (len <= 0.001f)
         return baseVolume;
   
      toSound.normalize();
      float angleDeg = acosf(toSound.dotProduct(listenerLook)) * (180.0f / 3.14159265f);
      if (angleDeg > 180.0f)
         angleDeg = 360.0f - angleDeg;
   
      float finalVolume = baseVolume;
   
      // Rear attenuation
      if (angleDeg > soundReductionAngle)
      {
         float range = 180.0f - soundReductionAngle;
         float attenProgress = (angleDeg - soundReductionAngle) / range;
         float volumeScale = 1.0f - (1.0f - soundVolumeRear) * attenProgress;
         finalVolume *= volumeScale;
      }
   
      // Side spike correction
      float halfCone = rightAngleCone * 0.5f;
      float lower = 90.0f - halfCone;
      float upper = 90.0f + halfCone;
   
      if (angleDeg >= lower && angleDeg <= upper)
      {
         float sideFadeProgress = 1.0f - fabsf(angleDeg - 90.0f) / halfCone;
         float spikeScale = 1.0f - (1.0f - rightAngleAdjustment) * sideFadeProgress * sideFadeProgress * sideFadeProgress;
         finalVolume *= spikeScale;
      }
   
      return finalVolume;
   }

   ControllerSound::ControllerSound()
   {
      soundEngine     = nullptr;
      listenerNode    = nullptr;
      backgroundMusic = nullptr;
      tickWadingPlayed = 0;
      lastListenerPosition = V3(0.0f, 0.0f, 0.0f);
   };

   void ControllerSound::Initialize()
   {
      if (IsInitialized)
         return;

      ISoundDeviceList* deviceList = ::irrklang::createSoundDeviceList();
      ik_s32 devicecount = deviceList->getDeviceCount();

      // we need at least one outputdriver (this can be a NULL device!)
      if (devicecount > 0)
      {
         // sound engine options, may be adjusted
         E_SOUND_ENGINE_OPTIONS options = (E_SOUND_ENGINE_OPTIONS)
            (E_SOUND_ENGINE_OPTIONS::ESEO_MULTI_THREADED |
            E_SOUND_ENGINE_OPTIONS::ESEO_LOAD_PLUGINS |
            E_SOUND_ENGINE_OPTIONS::ESEO_USE_3D_BUFFERS);

         E_SOUND_OUTPUT_DRIVER driver = E_SOUND_OUTPUT_DRIVER::ESOD_AUTO_DETECT;

         // try to initialize irrKlang
         soundEngine = ::irrklang::createIrrKlangDevice(driver, options);

         // if engine is initialized 
         // this is not null, it's null if no sound device in device manager
         if (soundEngine)
         {
            // sound engine properties, units are fine units with a melee range of 128 (= 2 grid units)
            // creates a stable 100% bubble of volume immediately around the sound source.
            soundEngine->setDefault3DSoundMinDistance(64.0f);
            // Sounds won't fall off past a grid distance of 50
            soundEngine->setDefault3DSoundMaxDistance(3200.0f);
            // Natural, but somewhat aggressive falloff to make distance from the source more meaningful
            soundEngine->setRolloffFactor(0.5f);
         }
      }
      else
         Logger::Log("SoundController", LogType::Warning, "No sound device found!");

      // cleanup
      deviceList->drop();

      // mark initialized
      IsInitialized = true;
   };

   // called each game tick
   void ControllerSound::Update()
   {
      if (!IsInitialized || !soundEngine)
         return;
   
      // Listener position and direction
      if (!listenerNode || !listenerNode->SceneNode || !listenerNode->RoomObject)
         return;
   
      ::Ogre::Vector3 pos = listenerNode->SceneNode->getPosition();
      double angle = listenerNode->RoomObject->Angle;
      V2 dir = MathUtil::GetDirectionForRadian(angle);
   
      vec3df listenerPos((ik_f32)pos.x, (ik_f32)pos.y, (ik_f32)-pos.z);
      vec3df listenerDir((ik_f32)dir.X, 0.0f, (ik_f32)-dir.Y);
   
      for (auto it = sharedSounds.begin(); it != sharedSounds.end(); )
      {
         if (it->Sound->isFinished()) {
            it->Sound->drop();
            it = sharedSounds.erase(it);
            continue;
         }
   
         it->Sound->setVolume(GetAttenuatedVolume(listenerPos, listenerDir, it->IsSelfOrigin, it->Position, it->BaseVolume));
   
         ++it;
      }
   }

   void ControllerSound::Destroy()
   {
      if (!IsInitialized)
         return;

      // cleanup and release of resources in our list of shared sounds
      for (auto it = sharedSounds.begin(); it != sharedSounds.end(); ++it)
         it->Sound->drop();

      sharedSounds.clear();
   
      if (soundEngine)
      {
         soundEngine->stopAllSounds();
         soundEngine->drop();	
      }

      if (listenerNode != nullptr)
      {
         listenerNode->RoomObject->PropertyChanged -= 
            gcnew PropertyChangedEventHandler(&OnRoomObjectPropertyChanged);
      }

      soundEngine     = nullptr;
      listenerNode    = nullptr;
      backgroundMusic = nullptr;

      // mark not initialized
      IsInitialized = false;
   };

   void ControllerSound::SetListenerNode(RemoteNode^ AvatarNode)
   {
      if (!IsInitialized || !soundEngine)
         return;

      // possibly detach old listener
      if (listenerNode)
      {
         listenerNode->RoomObject->PropertyChanged -= 
            gcnew PropertyChangedEventHandler(&OnRoomObjectPropertyChanged);
      }

      // set new value
      listenerNode = AvatarNode;

      // if not null
      if (listenerNode)
      {
         // hook up listener
         listenerNode->RoomObject->PropertyChanged += 
            gcnew PropertyChangedEventHandler(&OnRoomObjectPropertyChanged);

         // initial set
         UpdateListener(listenerNode);
      }
   };

   void ControllerSound::OnRoomObjectPropertyChanged(Object^ sender, PropertyChangedEventArgs^ e)
   {
      if(CLRString::Equals(e->PropertyName, RoomObject::PROPNAME_ANGLE) ||
         CLRString::Equals(e->PropertyName, RoomObject::PROPNAME_POSITION3D))
      {
         // update listener if listener object changed position or orientation
         UpdateListener(listenerNode);
      }
   };

   void ControllerSound::UpdateListener(RemoteNode^ AvatarNode)
   {
      if (!IsInitialized || !soundEngine || !AvatarNode || !AvatarNode->SceneNode)
         return;

      RoomObject^ avatar = OgreClient::Singleton->Data->AvatarObject;

      V3 pos = avatar->Position3D;
      V2 dir = MathUtil::GetDirectionForRadian(avatar->Angle);

      vec3df irrpos;
      irrpos.X = (ik_f32)pos.X;
      irrpos.Y = (ik_f32)pos.Y;
      irrpos.Z = (ik_f32)-pos.Z;

      vec3df irrlook;
      irrlook.X = (ik_f32)dir.X;
      irrlook.Y = 0.0f;
      irrlook.Z = (ik_f32)-dir.Y;

      soundEngine->setListenerPosition(irrpos, irrlook);

      // leaf avatar is in
      RooSubSector^ leaf = avatar->SubSector;

      // check for water sector wading sound
      if (leaf != nullptr && leaf->Sector != nullptr && lastListenerPosition != pos)
      {
         V2 pos2D = avatar->Position2D; // get avatar position
         pos2D.ConvertToROO();          // convert to ROO

         // get floor texture height at avatar position and convert back to world
         Real hFloor = 0.0625f * leaf->Sector->CalculateFloorHeight(pos2D.X, pos2D.Y, false);

         // shortcuts
         GameTickOgre^ tick = OgreClient::Singleton->GameTick;
         RooSectorFlags^ flags = leaf->Sector->Flags;

         // below floor, sector with depth and delay passed
         if (pos.Y < hFloor && flags->SectorDepth != RooSectorFlags::DepthType::Depth0 &&
             (tick->Current - tickWadingPlayed > 500*(unsigned int)flags->SectorDepth))
         {
            // get roominfo which stores wading info
            RoomInfo^ roomInfo = OgreClient::Singleton->Data->RoomInformation;

            // the soundinfo to play
            PlaySound^ soundInfo  = gcnew PlaySound();
            
            // set so it's replayed as avatar attached sound
            soundInfo->ID = 0;
            soundInfo->Row = 0;
            soundInfo->Column = 0;

            // set filename and resource
            soundInfo->ResourceName = roomInfo->WadingSoundFile;
            soundInfo->Resource = roomInfo->ResourceWadingSound;

            // play wading sound
            StartSound(soundInfo);

            // save tick we played it
            tickWadingPlayed = tick->Current;
         }
      }

      // save last listener position
      lastListenerPosition = pos;
   };

   void ControllerSound::AdjustMusicVolume()
   {
      if (!backgroundMusic)
         return;

      backgroundMusic->setVolume(OgreClient::Singleton->Config->MusicVolume / 10.0f);
   };

   void ControllerSound::AdjustSoundVolume()
   {
      for (auto it = sharedSounds.begin(); it != sharedSounds.end(); it++)
         it->Sound->setVolume(OgreClient::Singleton->Config->SoundVolume / 10.0f);

      for each(RoomObject^ obj in OgreClient::Singleton->Data->RoomObjects)
      {
         if (!obj->UserData)
            continue;

         RemoteNode^ node = (RemoteNode^)obj->UserData;

         if (!node || !node->Sounds)
            return;

         for (std::list<ISound*>::iterator it = node->Sounds->begin(); it != node->Sounds->end(); it++)
            (*it)->setVolume(OgreClient::Singleton->Config->SoundVolume / 10.0f);
      }
   };

   void ControllerSound::UpdateSoundVolumes(std::list<ISound*>* sounds, const ::Ogre::Vector3& soundWorldPos)
   {
      if (!listenerNode || !listenerNode->SceneNode || !listenerNode->RoomObject || !sounds)
         return;
   
      ::Ogre::Vector3 listenerOgrePos = listenerNode->SceneNode->getPosition();
      double angle = listenerNode->RoomObject->Angle;
      V2 dir = MathUtil::GetDirectionForRadian(angle);
   
      vec3df listenerPos((ik_f32)listenerOgrePos.x, (ik_f32)listenerOgrePos.y, (ik_f32)-listenerOgrePos.z);
      vec3df listenerDir((ik_f32)dir.X, 0.0f, (ik_f32)-dir.Y);
   
      vec3df soundPos((ik_f32)soundWorldPos.x, (ik_f32)soundWorldPos.y, (ik_f32)-soundWorldPos.z);
   
      float baseVolume = OgreClient::Singleton->Config->SoundVolume / 10.0f;
      float volume = GetAttenuatedVolume(listenerPos, listenerDir, false, soundPos, baseVolume);
   
      for (auto it = sounds->begin(); it != sounds->end(); ++it)
      {
         ISound* sound = *it;
         if (sound)
            sound->setVolume(volume);
      }
   }
   

   void ControllerSound::HandleGameModeMessage(GameModeMessage^ Message)
   {
      if (!IsInitialized || !soundEngine)
         return;

      switch ((MessageTypeGameMode)Message->PI)
      {
         case MessageTypeGameMode::Player:
            HandlePlayerMessage((PlayerMessage^)Message);
            break;

         case MessageTypeGameMode::PlayWave:
            HandlePlayWaveMessage((PlayWaveMessage^)Message);
            break;
#if !VANILLA
         case MessageTypeGameMode::StopWave:
            HandleStopWaveMessage((StopWaveMessage^)Message);
            break;
#endif
         case MessageTypeGameMode::PlayMusic:
            HandlePlayMusicMessage((PlayMusicMessage^)Message);
            break;

         case MessageTypeGameMode::PlayMidi:
            HandlePlayMidiMessage((PlayMidiMessage^)Message);
            break;
      }
   };

#if !VANILLA
   void ControllerSound::HandleStopWaveMessage(StopWaveMessage^ Message)
   {
      if (!IsInitialized || !soundEngine || !Message->PlayInfo->Resource || !Message->PlayInfo->ResourceName)
         return;

      // get resource name of wav file
      CLRString^ sourcename = Message->PlayInfo->ResourceName->ToLower();

      // native string
      ::Ogre::String& o_str = StringConvert::CLRToOgre(sourcename);
      const char* c_str = o_str.c_str();

      // check if sound is known to irrklang
      ISoundSource* soundsrc = soundEngine->getSoundSource(c_str, false);

      // Return if not found
      if (!soundsrc)
         return;

      // Source given by object id
      if (Message->PlayInfo->ID > 0)
      {
         // try get source object
         RoomObject^ source = OgreClient::Singleton->Data->RoomObjects->GetItemByID(Message->PlayInfo->ID);

         if (source && source->UserData)
         {
            // get attached remotenode
            RemoteNode^ node = (RemoteNode^)source->UserData;

            if (node && node->Sounds)
            {
               for (std::list<ISound*>::iterator it = node->Sounds->begin(); it != node->Sounds->end();++it)
               {
                  if ((*it)->getSoundSource() == soundsrc)
                  {
                     (*it)->stop();
                     (*it)->drop();
                     it = node->Sounds->erase(it);
                     return;
                  }
               }
            }
         }
      }

      // Check sharedSounds list
      for (auto it = sharedSounds.begin(); it != sharedSounds.end();++it)
      {
         if (it->Sound->getSoundSource() == soundsrc)
         {
            it->Sound->stop();
            it->Sound->drop();
            it = sharedSounds.erase(it);
            return;
         }
      }
      return;
   };
#endif

   void ControllerSound::HandlePlayerMessage(PlayerMessage^ Message)
   {
      if (!IsInitialized || !soundEngine)
         return;

      for (auto it = sharedSounds.begin(); it != sharedSounds.end(); )
      {
         // Preserve one shot sounds with self origin (e.g. door opening, level up sound etc.)
         if (!it->IsLooped && it->IsSelfOrigin)
         {
            ++it;
            continue;
         }

         it->Sound->stop();
         it->Sound->drop();
         it = sharedSounds.erase(it);
      }
   }

   void ControllerSound::HandlePlayMusicMessage(PlayMusicMessage^ Message)
   {
      StartMusic(Message->PlayInfo);
   };

   void ControllerSound::HandlePlayMidiMessage(PlayMidiMessage^ Message)
   {
      StartMusic(Message->PlayInfo);
   };

   void ControllerSound::HandlePlayWaveMessage(PlayWaveMessage^ Message)
   {
      StartSound(Message->PlayInfo);
   };

   void ControllerSound::StartSound(PlaySound^ Info)
   {
      if (!IsInitialized || !soundEngine || !Info || !Info->ResourceName || !Info->Resource)
         return;

      bool isLooped = Info->PlayFlags->IsLoop;

      if (isLooped && OgreClient::Singleton->Config->DisableLoopSounds)
         return;

      // Clean up finished sounds from the shared sound list
      for (auto it = sharedSounds.begin(); it != sharedSounds.end(); )
      {
         if (it->Sound->isFinished()) {
            it->Sound->drop();
            it = sharedSounds.erase(it);
         } else {
            ++it;
         }
      }

      const int MAX_ACTIVE_SOUNDS = 32;
      if ((int)sharedSounds.size() >= MAX_ACTIVE_SOUNDS) {
         TrackedSound& oldest = sharedSounds.front();
         oldest.Sound->stop();
         oldest.Sound->drop();
         sharedSounds.pop_front();
      }

      ////////////////////////////////////////////////////////////////////////////////////////////////////////////////

      // if source is a object, we save it here
      RemoteNode^ attachNode = nullptr;

      bool isSelfOrigin = false;

      // initial playback position
      float x = 0;
      float y = 0;
      float z = 0;

      // source given by object id
      if (Info->ID > 0)
      {
         // try get source object
         RoomObject^ source = OgreClient::Singleton->Data->RoomObjects->GetItemByID(Info->ID);

         if (source && source->UserData)
         {
            // get attached remotenode
            attachNode = (RemoteNode^)source->UserData;

            if (attachNode && attachNode->SceneNode)
            {
               ::Ogre::Vector3 pos = attachNode->SceneNode->getPosition();
               x = (float)pos.x;
               y = (float)pos.y;
               z = (float)-pos.z;
            }
         }
      }

      // source given by row/col
      else if (Info->Row > 0 && Info->Column > 0)
      {
         // convert the center of the server grid square to coords of roo file
         x = (float)(Info->Column - 1) * 1024.0f + 512.0f;
         z = (float)(Info->Row - 1) * 1024.0f + 512.0f;

         RooSubSector^ out;

         if (OgreClient::Singleton->CurrentRoom)
            y = (float)OgreClient::Singleton->CurrentRoom->GetHeightAt(x, z, out, true, false);

         // scale from roo to client and add 1 based num offset
         x = (x * 0.0625f) + 64.0f;
         y = (y * 0.0625f);
         z = -((z * 0.0625f) + 64.0f);
      }

      // source is own avatar
      else
      {
         // mark this as a self-origin sound
         isSelfOrigin = true;
      }

      ////////////////////////////////////////////////////////////////////////////////////////////////////////////////

      // native strings
      const ::Ogre::String& o_StrFull = StringConvert::CLRToOgre(Info->Resource);

      ISound* sound = nullptr;

      if (isSelfOrigin)
      {
         // play2D: full volume, not spatialized
         sound = soundEngine->play2D(
            o_StrFull.c_str(),
            isLooped,
            true,  // startPaused
            true); // track = true so we can drop it later
      }
      else
      {
         // try start 3D playback
         sound = soundEngine->play3D(
            o_StrFull.c_str(),
            vec3df(x, y, z),
            isLooped,
            true,
            true,
            ::irrklang::E_STREAM_MODE::ESM_AUTO_DETECT,
            false);
      }

      // success
      if (sound)
      {
         // retrieve our base volume based on settings
         float baseVolume = OgreClient::Singleton->Config->SoundVolume / 10.0f;

         if (ControllerSound::listenerNode &&
            ControllerSound::listenerNode->SceneNode &&
            ControllerSound::listenerNode->RoomObject)
         {
            // get listener data
            ::Ogre::Vector3 pos = ControllerSound::listenerNode->SceneNode->getPosition();
            double angle = ControllerSound::listenerNode->RoomObject->Angle;
            V2 dir = MathUtil::GetDirectionForRadian(angle);
            
            vec3df listenerPos((ik_f32)pos.x, (ik_f32)pos.y, (ik_f32)-pos.z);
            vec3df listenerDir((ik_f32)dir.X, 0.0f, (ik_f32)-dir.Y);

            // attenuate sound accordingly
            baseVolume = GetAttenuatedVolume(listenerPos, listenerDir, isSelfOrigin, vec3df(x, y, z), baseVolume);
         }

         sound->setVolume(baseVolume);

         // save reference to sound for adjusting (i.e. position)
         if (attachNode)
            attachNode->AddSound(sound);

         // unattached sounds are tracked in sharedSounds
         else
            sharedSounds.push_back(TrackedSound(sound, isSelfOrigin, isLooped, vec3df(x, y, z), baseVolume));       

         // start playback
         sound->setIsPaused(false);
      }
   }

   void ControllerSound::StartMusic(PlayMusic^ Info)
   {
      if (!IsInitialized || !soundEngine || !Info || !Info->ResourceName || !Info->Resource)
         return;

      if (OgreClient::Singleton->Config->MusicVolume > 0.0f)
      {
         const ::Ogre::String& name = StringConvert::CLRToOgre(Info->Resource);

         // no current background music
         if (!backgroundMusic)
         {
            backgroundMusic = soundEngine->play2D(
               name.c_str(), true, true, true, ::irrklang::E_STREAM_MODE::ESM_AUTO_DETECT, false);

            if (backgroundMusic)
            {
               backgroundMusic->setVolume(OgreClient::Singleton->Config->MusicVolume / 10.0f);
               backgroundMusic->setIsPaused(false);
            }
         }

         // stop old background music if another one is to be played
         else if (name != backgroundMusic->getSoundSource()->getName())
         {
            backgroundMusic->stop();
            backgroundMusic->drop();

            backgroundMusic = soundEngine->play2D(
               name.c_str(), true, true, true, ::irrklang::E_STREAM_MODE::ESM_AUTO_DETECT, false);

            if (backgroundMusic)
            {
               backgroundMusic->setVolume(OgreClient::Singleton->Config->MusicVolume / 10.0f);
               backgroundMusic->setIsPaused(false);
            }
         }
      }
   };
};};
