README – Assignment 3: TA Simulation (Shared Memory & Semaphores)

Author(s): Harnoor Kamboj and Pulcherie M'Baye

Course: SYSC 4001 (Operating Systems)

## FILES INCLUDED

ta_sim.c: Main C program for the TA simulation

exams/: Directory containing all exam text files

rubric.txt: Rubric file used and modified during execution

## COMPILATION INSTRUCTIONS

To compile the program, run the following command in the terminal:

gcc ta_sim.c -o ta_sim -lrt -pthread

This creates an executable file named “ta_sim”.

## RUNNING THE PROGRAM

General format:

./ta_sim <num_TAs> <exams_directory> <rubric_file> [--sync]

Where:

<num_TAs>         = Number of TA processes to create

<exams_directory> = Folder containing exam files

<rubric_file>     = Path to rubric text file

--sync            = (Optional) Enables synchronization using semaphores

## TEST CASES

1. Running WITHOUT synchronization:

./ta_sim 3 exams/ rubric.txt

Expected behaviour:

* Multiple TA processes run concurrently
* Race conditions may occur
* Rubric values may be overwritten by other processes
* All exams are eventually marked

2. Running WITH synchronization:

./ta_sim 3 exams/ rubric.txt --sync

Expected behaviour:

* Semaphores are used to protect shared memory
* No race conditions occur when updating the rubric or exams
* All exams are marked in a controlled order

## END OF README
