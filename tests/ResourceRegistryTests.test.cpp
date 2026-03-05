#include "ResourceManager.h"

#include "gtest/gtest.h"
#include <gtest/gtest.h>
#include <thread>

using namespace resmgr;

/**
 * Ensure that resources can be created and accessed on a single thread.
 */
TEST(ResourceRegistryTests, TestSingleThreadedAddResources)
{
   auto& intRegistry = ResourceRegistry<int>::GetInstance();
   const uint64_t token69 =
      intRegistry.Add(std::make_unique<int>(69), "single_threaded_add_and_get_69");
   const uint64_t token420 =
      intRegistry.Add(std::make_unique<int>(420), "single_threaded_add_and_get_420");

   // Ensure the tokens we got back are correct.
   ASSERT_EQ(token69, intRegistry.GetResourceToken("single_threaded_add_and_get_69"));
   ASSERT_EQ(token420, intRegistry.GetResourceToken("single_threaded_add_and_get_420"));

   // Get access separately, and compare values
   ReadOnlyResourceAccess<int> res69;
   intRegistry.TryGetAccess(token69, res69);
   ASSERT_EQ(*res69.GetResource(), 69);

   // Should be null after free.
   res69.FreeResource();
   ASSERT_EQ(res69.GetResource(), nullptr);

   ReadOnlyResourceAccess<int> res420;
   intRegistry.TryGetAccess(token420, res420);
   ASSERT_EQ(*res420.GetResource(), 420);

   // Should be null after free.
   res420.FreeResource();
   ASSERT_EQ(res420.GetResource(), nullptr);

   // Acquire both resources at the same time
   intRegistry.TryGetAccess(token69, res69);
   intRegistry.TryGetAccess(token420, res420);

   ASSERT_EQ(*res69.GetResource(), 69);
   ASSERT_EQ(*res420.GetResource(), 420);

   res69.FreeResource();
   res420.FreeResource();
}

/**
 * Resource access objects are meant to automatically be freed when the destructor is called as per
 * the RAII structure. Test that here.
 */
TEST(ResourceRegistryTests, TestSingleThreadedDestructor)
{

   // Test query access automatically freed.
   {
      auto& doubleRegistry = ResourceRegistry<double>::GetInstance();
      const uint64_t token6_9 =
         doubleRegistry.Add(std::make_unique<double>(6.9), "single_threaded_add_and_get_6.9");
      ReadOnlyResourceAccess<double> res6_9;
      doubleRegistry.TryGetAccess(token6_9, res6_9);
      ASSERT_EQ(*res6_9.GetResource(), 6.9);
   }
}

/**
 * Ensure we can get a catchable exception if two resources with the same name were created.
 */
TEST(ResourceRegistryTests, SingleThreadedAddDuplicateName)
{
   auto& intRegistry = ResourceRegistry<int>::GetInstance();
   intRegistry.Add(std::make_unique<int>(42), "single_threaded_add_duplicate_name");
   uint64_t duplicateToken;

   EXPECT_DEATH(
      {
         try
         {
            ASSERT_DEATH(duplicateToken = intRegistry.Add(
                            std::make_unique<int>(99), "single_threaded_add_duplicate_name"),
               ".*");
         }
         catch (...)
         {
            // Crash
            std::_Exit(1);
         }
      },
      "");
}

/**
 * Test several threads reading from the same resource at the same time.
 */
TEST(ResourceRegistryTests, MultiThreadedConcurrentReads)
{
   auto& intRegistry = ResourceRegistry<int>::GetInstance();
   const uint64_t token =
      intRegistry.Add(std::make_unique<int>(42), "mult_threaded_concurrent_reads");

   std::atomic_bool success(true);
   auto readTask = [&]
   {
      for (int i = 0; i < 100'000; ++i)
      {
         ReadOnlyResourceAccess<int> resource;
         intRegistry.TryGetAccess(token, resource);

         if (*resource.GetResource() != 42)
         {
            success = false;
            break;
         }
      }
   };

   // Read the resource from 3 separate threads at once.
   std::thread t1(readTask);
   std::thread t2(readTask);
   std::thread t3(readTask);
   t1.join();
   t2.join();
   t3.join();

   ASSERT_TRUE(success);
}

/**
 * Test a system where a resource is being written to with another thread trying to read from it at
 * the same time.
 */
TEST(ResourceRegistryTests, MultiThreadedWriteWithReadContention)
{
   auto& intRegistry = ResourceRegistry<int>::GetInstance();
   const uint64_t token =
      intRegistry.Add(std::make_unique<int>(42), "multi_threaded_write_with_read_contention");

   auto readTask = [&]
   {
      for (int i = 0; i < 50000; ++i)
      {
         ReadOnlyResourceAccess<int> resource;
         intRegistry.TryGetAccess(token, resource);
      }
   };

   constexpr int NumAdds = 50000;
   auto writeTask = [&]
   {
      for (int i = 0; i < NumAdds; ++i)
      {
         ReadWriteResourceAccess<int> resource;
         intRegistry.TryGetAccess(token, resource);
         *resource.GetResource() += 1;
      }
   };

   std::thread t1(readTask);
   std::thread t2(writeTask);
   t1.join();
   t2.join();

   ReadOnlyResourceAccess<int> resource;
   intRegistry.TryGetAccess(token, resource);
   ASSERT_EQ(*resource, NumAdds + 42);
}

/**
 * Test a situation where there's high contention between two threads.
 */
TEST(ResourceRegistryTests, MultiThreadedHighContention)
{
   auto& intRegistry = ResourceRegistry<int>::GetInstance();
   const uint64_t token =
      intRegistry.Add(std::make_unique<int>(0), "multi_threaded_high_contention");

   std::atomic_int completedReads(0);
   std::atomic_int completedWrites(0);

   constexpr int NumReads = 10000;
   constexpr int NumWrites = 5000;

   auto readTask = [&]
   {
      for (int i = 0; i < NumReads; ++i)
      {
         ReadOnlyResourceAccess<int> resource;
         intRegistry.TryGetAccess(token, resource);
         ++completedReads;
      }
   };

   auto writeTask = [&]
   {
      for (int i = 0; i < NumWrites; ++i)
      {
         ReadWriteResourceAccess<int> resource;
         intRegistry.TryGetAccess(token, resource);
         *resource = *resource + 1;
         ++completedWrites;
      }
   };

   std::thread t1(readTask);
   std::thread t2(writeTask);
   t1.join();
   t2.join();

   ASSERT_EQ(completedReads, NumReads);
   ASSERT_EQ(completedWrites, NumWrites);
}

struct tTestObjectOperatorsObj
{
   tTestObjectOperatorsObj(const int testInt, const double testDouble)
      : TestInt(testInt), TestDouble(testDouble)
   {
   }

   int TestInt;
   double TestDouble;
};

TEST(ResourceRegistryTests, TestObjectOperators)
{
   auto& registry = ResourceRegistry<tTestObjectOperatorsObj>::GetInstance();
   const uint64_t token = registry.Add(
      std::make_unique<tTestObjectOperatorsObj>(420, 6.9), "test_object_operators_obj");

   {
      ReadOnlyResourceAccess<tTestObjectOperatorsObj> resource;
      ResourceRegistry<tTestObjectOperatorsObj>::GetInstance().TryGetAccess(token, resource);

      // Test the -> and * operators on the read only access object.
      ASSERT_EQ((*resource).TestDouble, 6.9);
      ASSERT_EQ(resource->TestDouble, 6.9);
      ASSERT_EQ((*resource).TestInt, 420);
      ASSERT_EQ(resource->TestInt, 420);
   }

   // Test read write access
   {
      ReadWriteResourceAccess<tTestObjectOperatorsObj> resource;
      ResourceRegistry<tTestObjectOperatorsObj>::GetInstance().TryGetAccess(token, resource);

      resource->TestDouble = 100;
      resource->TestInt = 100;

      // Test the -> and * operators on the read only access object.
      ASSERT_EQ((*resource).TestDouble, 100);
      ASSERT_EQ(resource->TestDouble, 100);
      ASSERT_EQ((*resource).TestInt, 100);
      ASSERT_EQ(resource->TestInt, 100);
   }
}

int main(int argc, char** argv)
{
   testing::InitGoogleTest(&argc, argv);
   return RUN_ALL_TESTS();
}