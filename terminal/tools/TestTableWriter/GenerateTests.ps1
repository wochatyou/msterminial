# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.

################################################################################
# This script generates the an array of UiaTextRange tests suitable for replacing the body of
# src\interactivity\win32\ut_interactivity_win32\UiaTextRangeTests.cpp TEST_METHOD(GeneratedMovementTests)
#
# See tools\TestTableWriter\README.md for more details on how to use this script.

[CmdletBinding()]
Param(
    [Parameter(Position=0, ValueFromPipeline=$true)]
    [string]$TestPath = "$PSScriptRoot/../../tools/TestTableWriter/UiaTests.csv"
)

# 0. Generate a comment telling people to not modify these tests in the .cpp
$result = "// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// DO NOT MODIFY THESE TESTS DIRECTLY
// These were generated by tools\TestTableWriter\GenerateTests.ps1
// Read tools\TestTableWriter\README.md for more details"

# 1. Define a few helpful variables to make life easier.
$result += "
// Define a few helpful variables
constexpr til::rect bufferSize{ 0, 0, 80, 300 };
constexpr til::CoordType midX{ 40 };
constexpr til::CoordType midY{ 150 };
constexpr til::CoordType midPopulatedY{ 75 };
constexpr til::CoordType segment0{ 0 };
constexpr til::CoordType segment1{ 16 };
constexpr til::CoordType segment2{ 32 };
constexpr til::CoordType segment3{ 48 };
constexpr til::CoordType segment4{ 64 };
constexpr til::point origin{ 0, 0 };
constexpr til::point midTop{ midX, 0 };
constexpr til::point midHistory{ midX, midPopulatedY };
constexpr til::point midDocEnd{ midX, midY };
constexpr til::point lastCharPos{ 72, midY };
constexpr til::point docEnd{ 0, midY + 1 };
constexpr til::point midEmptySpace{ midX, midY + midPopulatedY };
constexpr til::point bufferEnd{ 79, 299 };
constexpr til::point endExclusive{ 0, 300 };`n"

# 2. Import the CSV test file and find all of the variables we need
$tests = Import-Csv $TestPath;
$vars = New-Object System.Collections.Generic.SortedSet[string];
foreach ($test in $tests)
{
    $vars.Add($test.Start) > $null;
    $vars.Add($test.End) > $null;
    $vars.Add($test.Result_Start) > $null;
    $vars.Add($test.Result_End) > $null;
}

# 3. Define each of the vars
# 3.a. Some of the variables were already defined at the beginning. So let's remove those.
$vars.Remove("origin") > $null;
$vars.Remove("midTop") > $null;
$vars.Remove("midHistory") > $null;
$vars.Remove("midDocEnd") > $null;
$vars.Remove("lastCharPos") > $null;
$vars.Remove("docEnd") > $null;
$vars.Remove("midEmptySpace") > $null;
$vars.Remove("bufferEnd") > $null;
$vars.Remove("endExclusive") > $null;

# 3.b. Now all of the remaining vars can be deduced from standard vars
foreach ($var in $vars)
{
    # Figure out what heuristic to use
    $segmentHeuristic = $var.Contains("segment");
    $leftHeuristic = $var.Contains("Left");
    $movementHeuristic = $var -match ".*\d+.*";

    # i. Contains "segment" --> define point at the beginning of a text segment
    if ($segmentHeuristic)
    {
        $result += "constexpr til::point {0}{{ {1}, {2}.y }};" -f $var, $var.Substring(0, 8), $var.Substring(9, $var.Length - $var.IndexOf("L") - 1);
    }
    # ii. Contains number --> requires movement
    elseif ($movementHeuristic)
    {
        # everything excluding last 3 characters denotes the standard variable
        # we're based on.
        $standardVar = $var.Substring(0, $var.length - 3);

        # 3rd to last character denotes the movement direction
        # P --> plus/forwards
        # M --> minus/backwards
        $moveForward = $var.substring($var.length - 3, 1) -eq 'P';

        # 2nd to last character denotes the movement amount
        $moveAmt = $var.substring($var.length -2, 1);

        # last character denotes the movement type
        switch ($var.substring($var.length - 1, 1)) {
            'C' # move by character
            {
                if ($moveForward)
                {
                    $result += "constexpr auto {0}{{ point_offset_by_char({1}, bufferSize, {2}) }};" -f $var, $standardVar, $moveAmt;
                }
                else
                {
                    $result += "constexpr auto {0}{{ point_offset_by_char({1}, bufferSize, -{2}) }};" -f $var, $standardVar, $moveAmt;
                }
            }
            'L' # move by line
            {
                if ($moveForward)
                {
                    $result += "constexpr auto {0}{{ point_offset_by_line({1}, bufferSize, {2}) }};" -f $var, $standardVar, $moveAmt;
                }
                else
                {
                    $result += "constexpr auto {0}{{ point_offset_by_line({1}, bufferSize, -{2}) }};" -f $var, $standardVar, $moveAmt;
                }
            }
            Default { Write-Host "Error: unknown variable movement type" -ForegroundColor Red }
        }
    }
    # iii. Contains "Left" --> set X to left
    elseif ($leftHeuristic)
    {
        $standardVar = $var.Split("Left")[0]
        $result += "constexpr til::point {0}{{ bufferSize.left, {1}.y }};" -f $var, $standardVar;
    }
    $result += "`n";
}

# 4. Write the tests
# 4.a. Introduce a struct to store each test as
$result += "struct GeneratedMovementTestInput
{
    TextUnit unit;
    int moveAmount;
    til::point start;
    til::point end;
};
struct GeneratedMovementTestExpected
{
    int moveAmount;
    til::point start;
    til::point end;
};
struct GeneratedMovementTest
{
    std::wstring_view name;
    GeneratedMovementTestInput input;
    GeneratedMovementTestExpected expected;
    bool skip;
};`n`n";

# 4.b. Iterate through CSV file and generate a test for each one
$result += "static constexpr std::array<GeneratedMovementTest, {0}> s_movementTests`n{{`n" -f $tests.count;
foreach ($test in $tests)
{
    $degeneratePrefix = $test.degenerate -eq "TRUE" ? "" : "non-";
    $movementType = $test.TextUnit.substring(9);
    $testName = "L`"Move {0}degenerate range at position {1} {2} times by {3}`"" -f $degeneratePrefix, $test.Position, $test.MoveAmount, $movementType;
    $testInput = "GeneratedMovementTestInput{{
            TextUnit::{0},
            {1},
            {2},
            {3}
        }}" -f $test.TextUnit, $test.MoveAmount, $test.Start, $test.End;
    $testExpected = "GeneratedMovementTestExpected{{
            {0},
            {1},
            {2}
        }}" -f $test.Result_MoveAmount, $test.Result_Start, $test.Result_End;
    $skip = $test.Skip -eq "TRUE" ? "true" : "false";

    $result += "    GeneratedMovementTest{{
        {0},
        {1},
        {2},
        {3}
    }},`n" -f $testName, $testInput, $testExpected, $skip;
}
$result += "};`n`n"

$result > "$PSScriptRoot/../../src/interactivity/win32/ut_interactivity_win32/GeneratedUiaTextRangeMovementTests.g.cpp";
