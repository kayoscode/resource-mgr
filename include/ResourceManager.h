#pragma once

#include <atomic>
#include <cassert>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace resmgr
{
/**
 * Interface to add to classes that don't support copying.
 */
class NoCopy
{
public:
   virtual ~NoCopy() = default;
   NoCopy()
   {
   }

   NoCopy(const NoCopy&) = delete;
   NoCopy& operator=(const NoCopy&) = delete;
};

/**
 * Handles tracking which unique ids have previously been assigned. Generates a new one.
 * It is invalid to use more than uint64_t::Max UIDS.
 */
class UidLong
{
public:
   /**
    * Standard constructor.
    */
   UidLong() : mCurrentId(0)
   {
   }

   /**
    * Returns the next id.
    */
   uint64_t NextId()
   {
      const uint64_t nextId = mCurrentId++;

      // Check wrap around.
      if (mCurrentId == 0 || mCurrentId == InvalidUid)
      {
         assert(false && "Too many unique Ids used. This should never happen.");
      }

      return nextId;
   }

   static constexpr uint64_t InvalidUid = static_cast<uint64_t>(-1);

private:
   std::atomic<unsigned long long> mCurrentId;
};

/**
 * Class for getting readonly access to a resource. At runtime, it's enforced that as many read
 * accesses can be used at once, but only a single write access.
 * @tparam T
 */
template <typename T> class ReadOnlyResourceAccess : NoCopy
{
public:
   ReadOnlyResourceAccess() : mResource(nullptr)
   {
   }

   ~ReadOnlyResourceAccess() override
   {
      // Return resource access when the object is destroyed.
      FreeResource();
   }

   /**
    * Clears the mutex and invalidates the pointer.
    */
   void FreeResource()
   {
      if (mResource != nullptr)
      {
         mResource = nullptr;
         mReadLock.reset();
      }
   }

   [[nodiscard]] const T* GetResource() const
   {
      return mResource;
   }

   /**
    * Sets the resource pointer. This function should be called only once during the lifetime of
    * this object. It should only be called within the resource manager.
    * @param pResource A pointer to the resource. The caller of this method must own the resource.
    * This class simply locks it till it goes out of scope
    * @param mutex The mutex lock for this resource object.
    */
   void LockResource(const T* pResource, std::shared_mutex& mutex)
   {
      mReadLock.emplace(mutex);
      mResource = pResource;
   }

   /**
    * Get the reference.
    */
   [[nodiscard]] const T& operator*()
   {
      return *mResource;
   }

   /**
    * Get the pointer
    */
   [[nodiscard]] const T* operator->()
   {
      return mResource;
   }

private:
   std::optional<std::shared_lock<std::shared_mutex>> mReadLock;
   const T* mResource;
};

template <typename T> class ReadWriteResourceAccess : NoCopy
{
public:
   ReadWriteResourceAccess() : mResource(nullptr)
   {
   }

   ~ReadWriteResourceAccess() override
   {
      // Return resource access when the object is destroyed.
      FreeResource();
   }

   /**
    * Clears the mutex and invalidates the pointer.
    */
   void FreeResource()
   {
      if (mResource != nullptr)
      {
         mResource = nullptr;
         mReadLock.reset();
      }
   }

   [[nodiscard]] T* GetResource() const
   {
      return mResource;
   }
   /**
    * Sets the resource pointer. This function should be called only once during the lifetime of
    * this object. It should only be called within the resource manager.
    * @param pResource A non-owned pointer to the underlying resource
    * @param mutex
    */
   void LockResource(T* pResource, std::shared_mutex& mutex)
   {
      mReadLock.emplace(mutex);
      mResource = pResource;
   }

   /**
    * Get the reference.
    */
   [[nodiscard]] T& operator*()
   {
      return *mResource;
   }

   /**
    * Get the pointer
    */
   [[nodiscard]] T* operator->()
   {
      return mResource;
   }

private:
   std::optional<std::unique_lock<std::shared_mutex>> mReadLock;
   T* mResource;
};

/**
 * Class which provides the ability to store static resources in a thread safe manner.
 * Static resources system:
 * Requirements:
 * 1. All access must be thread safe.
 * 2. Access must be signaled before modification
 * 3. Performance must be considered as access must always go through the static interface
 * 4. Should be extensible for any kind of type the user wants to implement when deriving the
 framework

 * Implementation details:
 * 0. Initially, items are added to the registry with a name, and that name can be used globally to
 * access the object.
 *         - If a client knows the name of their resource, they can speed up retrieval for future
 * accesses via a returned token that provides a direct index into the set of resources.
 * 1. Create a resource registry with a data structure optimized for retrieval.
 * 2. All objects in the registry must be unique ptrs.
 * 3. Provide two resource access modes.
 *         1. Read access: returns a const reference to the object
 *                  -> Allows an infinite number of read accesses at a single time
 *         2. Write access: returns a mutable reference to the object
 *                  -> Only a single write access can be returned. A condition variable is set when
 the
 *                         write access is given, and notified when the user stops accessing the
 * writable object

 * Important patterns:
 *         1. Resources should be accessed and freed in as small blocks as possible to avoid locking
 up
 * the system
 *                 -> Resource accesses should definitely not be stored in objects unless you
 created a
 * private resource manager for your class and really know what you're doing
 *         2. Once access is returned to the resource manager, The pointer should be considered
 * invalid. Access must be retrieved again for the object to be considered valid.
 */
template <typename T> class ResourceRegistry : NoCopy
{
public:
   static ResourceRegistry& GetInstance()
   {
      static ResourceRegistry resource;
      return resource;
   }

   /**
    * Adds a new resource to the registry with a given name.
    * @param newResource This class takes ownership of the unique pointer backing the resource
    * @param name The ascii tag/name given for the resource
    * @return The token that can be used to get quick access to the resource in the future without
    * going through the tag.
    */
   uint64_t Add(std::unique_ptr<T> newResource, const std::string& name)
   {
      auto lock((std::unique_lock<std::shared_mutex>(mMutex)));

      // Check if the resource already exists
      if (mOwnedResources.contains(name))
      {
         return UidLong::InvalidUid;
      }

      // Add the resource.
      const uint64_t newToken = mUidGen.NextId();

      // Set data
      mResources[newToken].SetResource(newResource.get());

      mOwnedResources[name] = std::make_tuple(std::move(newResource), newToken);
      return newToken;
   }

   /**
    * Finds the token for a given resource from the given name.
    * @param name The tag used when the resource was added to the registry.
    * @return The token that can be used for quick access of the resource.
    */
   uint64_t GetResourceToken(const std::string& name)
   {
      std::shared_lock lock(mMutex);

      // Check if the resource already exists
      auto it = mOwnedResources.find(name);
      if (it == mOwnedResources.end())
      {
         return UidLong::InvalidUid;
      }

      return std::get<1>(it->second);
   }

   /**
    * Retrieves a readonly reference to the resource. There can be as many readonly accesses as
    * desired during the program's runtime. When you have readonly access, you are taking temporary
    * ownership of the resource.
    */
   inline bool TryGetAccess(uint64_t token, ReadOnlyResourceAccess<T>& resourceAccess)
   {
      std::shared_lock lock(mMutex);
      auto it = mResources.find(token);
      if (it != mResources.end())
      {
         lock.unlock();
         it->second.GetAccess(resourceAccess);
         return true;
      }

      return false;
   }

   /**
    * Retrieves a readonly reference to the resource. There can be as many readonly accesses as
    * desired during the program's runtime. When you have readonly access, you are taking temporary
    * ownership of the resource.
    */
   inline bool TryGetAccess(uint64_t token, ReadWriteResourceAccess<T>& resourceAccess)
   {
      std::shared_lock lock(mMutex);
      auto it = mResources.find(token);
      if (it != mResources.end())
      {
         lock.unlock();
         it->second.GetAccess(resourceAccess);
         return true;
      }

      return false;
   }

private:
   /**
    * Structure storing resource access data used for enforcing access rules.
    */
   struct tResourceAccessData final
   {
      tResourceAccessData() : mResource(nullptr)
      {
      }

      /**
       * Should be called when the resource gets added.
       */
      void SetResource(T* resource)
      {
         mResource = resource;
      }

      /**
       * Holds the thread until it successfully acquires a readonly pointer to the underlying
       * object.
       */
      inline void GetAccess(ReadOnlyResourceAccess<T>& accessObject)
      {
         // Lock our mutex with the shared lock inside the access object.
         accessObject.LockResource(mResource, mMutex);
      }

      /**
       * Holds the thread until it successfully acquires a read/write pointer to the underlying
       * object.
       */
      inline void GetAccess(ReadWriteResourceAccess<T>& accessObject)
      {
         // Lock the unique lock inside the access object, hold it until our client is ready to dump
         // it.
         accessObject.LockResource(mResource, mMutex);
      }

   private:
      T* mResource;

      // The mutex to use for reader/writer locks to the underlying data.
      std::shared_mutex mMutex;
   };

   /**
    * Private constructor for singleton instance.
    */
   ResourceRegistry() : mOwnedResources(), mResources()
   {
   }

   std::shared_mutex mMutex;

   // Generates unique ids for each resource.
   UidLong mUidGen;

   // Stores a mapping of names to the owned pointer of resources.
   std::map<std::string, std::tuple<std::unique_ptr<T>, uint64_t>> mOwnedResources;

   // Stores a vector of pointers to the resources. This is for quick look up by token.
   std::unordered_map<uint64_t, tResourceAccessData> mResources;
};
}   // namespace resmgr