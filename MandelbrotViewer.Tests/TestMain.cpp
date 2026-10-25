#include "pch.h"
#include "TestHarness.h"

int main(int argc, char* argv[])
{
    return TestHarness::TestRegistry::Instance().Run(argc, argv);
}
