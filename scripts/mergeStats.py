#!/usr/bin/python

import os
import csv
from sys import argv

from statistics import mean
from statistics import median
import math

# Usage: mergeStats.py <input_directory> <output_prefix>
# This script will merge all stats in the direcotry and write to the output
# files, one for average and one for median.
# Which means it will calculate the avergage and the mean number of each
# column and row.

LOG_EXT = "csv"
inputData = []

def ensure_parent(d):
    try:
        os.makedirs(os.path.dirname(d))
    except:
        pass

def read_csv(name):
    with open(name) as csvfile:
        return [x for x in csv.DictReader(csvfile)]

def write_csv(name, outputData):
    with open(name, 'w') as csvfile:
        fieldnames = [x for x in outputData[0]]
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)

        writer.writeheader()
        for data in outputData:
            writer.writerow(data)

def list_files(listDir = None, fileExt = None):
    if listDir == None:
        print('Please provide file directory!')
        exit()
    if fileExt == None:
        print('Please provide file extension: e.g., csv')
        exit()
    fileExt = '.{0}'.format(fileExt)
    outputFiles = [
        x for x in os.listdir(listDir) if x.endswith(fileExt)
    ]
    return outputFiles

def ensure_output(outputPath):
    ensure_parent(outputPath)
    # Make sure we don't merge the previous merged file
    try:
        os.remove(outputPath)
    except OSError:
        pass

def main():
    global inputData
    avgOutputData = []
    medOutputData = []

    if len(argv) < 3:
        print("Usage: ./mergeStats.py <input_directory> <output_prefix>")
        exit()

    fileDir = argv[1]
    outputPre = argv[2]
    outputAvg = outputPre + "_avg." + LOG_EXT
    outputPathAvg = os.path.join(fileDir, outputAvg)
    outputMed = outputPre + "_median." + LOG_EXT
    outputPathMed = os.path.join(fileDir, outputMed)

    ensure_output(outputPathAvg)
    ensure_output(outputPathMed)

    for fileName in list_files(fileDir, LOG_EXT):
        print("{:s}".format(fileName))
        filePath = os.path.join(fileDir, fileName)
        inputData.append(read_csv(filePath))

    # All log files should have the same number of lines
    lines = len(inputData[0])
    keys = [x for x in inputData[0][0]]
    for l in xrange(0, lines):
        tmpdata = {}
        tmpdata2 = {}
        for key in keys:
            tmpList = []
            for data in inputData:
                tmpData = 0.0
                try:
                    tmpData = float(data[l][key])
                except:
                    print("Not float! {:s}".format(data[l][key]))
                    continue
                if not math.isnan(tmpData):
                    tmpList.append(float(data[l][key]))
            if len(tmpList) > 0:
                if (key == "Skew") or (key == "Goal"):
                    tmpdata[key] = tmpList[0] # Just use the first one
                    tmpdata2[key] = tmpList[0]
                else:
                    tmpdata[key] = mean(tmpList)
                    tmpdata2[key] = median(tmpList)
            else:
                tmpdata[key] = float('nan')
                tmpdata2[key] = float('nan')
        avgOutputData.append(tmpdata)
        medOutputData.append(tmpdata2)

    write_csv(outputPathAvg, avgOutputData)
    write_csv(outputPathMed, medOutputData)

if __name__ == "__main__":
    main()
