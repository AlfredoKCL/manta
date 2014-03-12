// -*- mode: c++; indent-tabs-mode: nil; -*-
//
// Manta
// Copyright (c) 2013-2014 Illumina, Inc.
//
// This software is provided under the terms and conditions of the
// Illumina Open Source Software License 1.
//
// You should have received a copy of the Illumina Open Source
// Software License 1 along with this program. If not, see
// <https://github.com/sequencing/licenses/>
//

///
/// \author Chris Saunders
///

#include "SVCandidateProcessor.hh"
#include "manta/SVMultiJunctionCandidateUtil.hh"

#include "blt_util/log.hh"

#include "boost/foreach.hpp"

#include <iostream>


//#define DEBUG_GSV



SVWriter::
SVWriter(
    const GSCOptions& initOpt,
    const SVLocusSet& cset,
    const char* progName,
    const char* progVersion,
    TruthTracker& truthTracker) :
    opt(initOpt),
    isSomatic(! opt.somaticOutputFilename.empty()),
    svScore(opt, cset.header),
    candfs(opt.candidateOutputFilename),
    dipfs(opt.diploidOutputFilename),
    somfs(opt.somaticOutputFilename),
    candWriter(opt.referenceFilename,cset,candfs.getStream()),
    diploidWriter(opt.diploidOpt, (! opt.chromDepthFilename.empty()),
                  opt.referenceFilename,cset,dipfs.getStream()),
    somWriter(opt.somaticOpt, (! opt.chromDepthFilename.empty()),
              opt.referenceFilename,cset,somfs.getStream()),
    _truthTracker(truthTracker)
{
    if (0 == opt.edgeOpt.binIndex)
    {
        candWriter.writeHeader(progName, progVersion);
        diploidWriter.writeHeader(progName, progVersion);
        if (isSomatic) somWriter.writeHeader(progName, progVersion);
    }
}



static
bool
isAnyFalse(
    const std::vector<bool>& vb)
{
    BOOST_FOREACH(const bool val, vb)
    {
        if (! val) return true;
    }
    return false;
}



void
SVWriter::
writeSV(
    const EdgeInfo& edge,
    const SVCandidateSetData& svData,
    const std::vector<SVCandidateAssemblyData>& mjAssemblyData,
    const SVMultiJunctionCandidate& mjSV)
{
    const unsigned junctionCount(mjSV.junction.size());

    // track filtration for each junction:
    std::vector<bool> isJunctionFiltered(junctionCount,false);

    // early SV filtering:
    //
    // 2 junction filter types:
    // 1) tests where the junction can fail independently
    // 2) tests where all junctions have to fail for the candidate to be filtered:

    bool isCandidateSpanFail(true);

    for (unsigned junctionIndex(0); junctionIndex<junctionCount; ++junctionIndex)
    {
        const SVCandidateAssemblyData& assemblyData(mjAssemblyData[junctionIndex]);
        const SVCandidate& sv(mjSV.junction[junctionIndex]);

        const bool isCandidateSpanning(assemblyData.isCandidateSpanning);

    #ifdef DEBUG_GSV
        log_os << __FUNCTION__ << ": isSpanningSV junction: " <<  isCandidateSpanning << "\n";
    #endif

        // junction dependent tests:
        bool isJunctionSpanFail(false);
        if (isCandidateSpanning)
        {
            static const unsigned minCandidateSpanningCount(3);
            if (sv.bp1.getSpanningCount() < minCandidateSpanningCount)
            {
                isJunctionSpanFail=true;
    #ifdef DEBUG_GSV
                log_os << logtag << "Rejecting candidate: minCandidateSpanningCount\n";
    #endif
                _truthTracker.reportOutcome(SVLog::LOW_SPANNING_COUNT);
                return;
            }
        }
        if (! isJunctionSpanFail) isCandidateSpanFail=false;

        // independent tests -- as soon as one of these fails, we can continue:
        //
        if (! isCandidateSpanning)
        {
            if (sv.isImprecise())
            {
                // in this case a non-spanning low-res candidate went into assembly but
                // did not produce a successful contig alignment:
    #ifdef DEBUG_GSV
                log_os << __FUNCTION__ << ": Rejecting candidate junction: imprecise non-spanning SV\n";
    #endif
                _truthTracker.reportOutcome(SVLog::IMPRECISE_NON_SPANNING);
                isJunctionFiltered[junctionIndex] = true;
                continue;
            }
        }

        // check min size for candidate output:
        if (isSVBelowMinSize(sv,opt.scanOpt.minCandidateVariantSize))
        {
    #ifdef DEBUG_GSV
            log_os << logtag << "Filtering out candidate below min size before candidate output stage\n";
    #endif
            return;
        }
    }

    // revisit dependent tests:
    //
    if (isCandidateSpanFail)
    {
        for (unsigned junctionIndex(0); junctionIndex<junctionCount; ++junctionIndex)
        {
#ifdef DEBUG_GSV
            log_os << __FUNCTION__ << ": Rejecting candidate junction: minCandidateSpanningCount\n";
#endif
            _truthTracker.reportOutcome(SVLog::LOW_SPANNING_COUNT);
            isJunctionFiltered[junctionIndex] = true;
        }
    }

    // check to see if all junctions are filtered, if so skip the whole candidate:
    //
    if (! isAnyFalse(isJunctionFiltered))
    {
#ifdef DEBUG_GSV
        log_os << __FUNCTION__ << ": Rejecting candidate, all junctions filtered.\n";
#endif
        return;
    }

    // write out candidates for each junction independently:
    for (unsigned junctionIndex(0); junctionIndex<junctionCount; ++junctionIndex)
    {
        if (isJunctionFiltered[junctionIndex]) continue;

        const SVCandidateAssemblyData& assemblyData(mjAssemblyData[junctionIndex]);
        const SVCandidate& sv(mjSV.junction[junctionIndex]);

        candWriter.writeSV(edge, svData, assemblyData, sv);
    }

    // check min size for scoring:
    for (unsigned junctionIndex(0); junctionIndex<junctionCount; ++junctionIndex)
    {
        if (isJunctionFiltered[junctionIndex]) continue;

        const SVCandidate& sv(mjSV.junction[junctionIndex]);
        if (isSVBelowMinSize(sv, opt.minScoredVariantSize))
        {
    #ifdef DEBUG_GSV
            log_os << logtag << "Filtering out candidate junction below min size at scoring stage\n";
    #endif
            isJunctionFiltered[junctionIndex] = true;
        }
    }

    // check to see if all junctions are filtered before scoring:
    //
    if (! isAnyFalse(isJunctionFiltered)) return;

    svScore.scoreSV(svData, mjAssemblyData, mjSV, isJunctionFiltered, isSomatic, mjModelScoreInfo);

    // final scored output is treated (mostly) independently for each junction:
    //
    for (unsigned junctionIndex(0); junctionIndex<junctionCount; ++junctionIndex)
    {
        if (isJunctionFiltered[junctionIndex]) continue;

        const SVCandidateAssemblyData& assemblyData(mjAssemblyData[junctionIndex]);
        const SVCandidate& sv(mjSV.junction[junctionIndex]);
        const SVModelScoreInfo& modelScoreInfo(mjModelScoreInfo[junctionIndex]);

        if (modelScoreInfo.diploid.altScore >= opt.diploidOpt.minOutputAltScore || opt.isRNA) /// TODO remove after adding RNA scoring
        {
            diploidWriter.writeSV(edge, svData, assemblyData, sv, modelScoreInfo);
        }

        if (isSomatic)
        {
            if (modelScoreInfo.somatic.somaticScore > opt.somaticOpt.minOutputSomaticScore)
            {
                somWriter.writeSV(edge, svData, assemblyData, sv, modelScoreInfo);
                _truthTracker.reportOutcome(SVLog::WRITTEN);
            }
            else
            {
                _truthTracker.reportOutcome(SVLog::LOW_SOMATIC_SCORE);
            }
        }
    }
}



SVCandidateProcessor::
SVCandidateProcessor(
    const GSCOptions& opt,
    const char* progName,
    const char* progVersion,
    const SVLocusSet& cset,
    TruthTracker& truthTracker,
    EdgeRuntimeTracker& edgeTracker) :
    _opt(opt),
    _truthTracker(truthTracker),
    _edgeTracker(edgeTracker),
    _svRefine(opt, cset.header),
    _svWriter(opt, cset, progName, progVersion, truthTracker)
{}



void
SVCandidateProcessor::
evaluateCandidate(
    const EdgeInfo& edge,
    const SVMultiJunctionCandidate& mjCandidateSV,
    const SVCandidateSetData& svData)
{
    assert(! mjCandidateSV.junction.empty());

    const unsigned junctionCount(mjCandidateSV.junction.size());

    if (_opt.isVerbose)
    {
        log_os << __FUNCTION__ << ": Starting analysis for SV candidate containing " << junctionCount << " junctions. Low-resolution junction candidate ids:";
        BOOST_FOREACH(const SVCandidate& sv, mjCandidateSV.junction)
        {
            log_os << " " << sv.candidateIndex;
        }
        log_os << "\n";
    }
#ifdef DEBUG_GSV
    log_os << __FUNCTION__ << ": CandidateSV: " << mjCandidateSV << "\n";
#endif


    const bool isComplex(isComplexSV(mjCandidateSV));
    _edgeTracker.addCand(isComplex);

    /// assemble each junction independently:
    std::vector<SVCandidateAssemblyData> mjAssemblyData(junctionCount);

    if (! _opt.isSkipAssembly)
    {
        for (unsigned junctionIndex(0); junctionIndex<junctionCount; ++junctionIndex)
        {
            const SVCandidate& candidateSV(mjCandidateSV.junction[junctionIndex]);
            SVCandidateAssemblyData& assemblyData(mjAssemblyData[junctionIndex]);
            _svRefine.getCandidateAssemblyData(candidateSV, svData, assemblyData, _opt.isRNA);

            if (_opt.isVerbose)
            {
                log_os << __FUNCTION__ << ": Candidate assembly complete for junction " << junctionIndex << "/" << junctionCount << ". Assembled candidate count: " << assemblyData.svs.size() << "\n";
            }

            if(! assemblyData.svs.empty())
            {
                const unsigned assemblyCount(assemblyData.svs.size());

                // can't be multi-junction and multi-assembly at the same time:
                assert(! ((junctionCount>1) && (assemblyCount>1)));

                // fill in assembly tracking data:
                _edgeTracker.addAssm(isComplex);
                _truthTracker.reportNumAssembled(assemblyData.svs.size());
                for (unsigned assemblyIndex(0); assemblyIndex<assemblyCount; ++assemblyIndex)
                {
                    _truthTracker.addAssembledSV();
                }
            }
        }
    }

    SVMultiJunctionCandidate mjAssembledCandidateSV;
    mjAssembledCandidateSV.junction.resize(junctionCount);

    std::vector<unsigned> junctionTracker(junctionCount,0);
    while(true)
    {
        bool isWrite(false);
        for (unsigned junctionIndex(0); junctionIndex<junctionCount; ++junctionIndex)
        {
            const SVCandidateAssemblyData& assemblyData(mjAssemblyData[junctionIndex]);
            unsigned& assemblyIndex(junctionTracker[junctionIndex]);

            if (assemblyData.svs.empty())
            {
                if (assemblyIndex != 0) continue;
#ifdef DEBUG_GSV
                log_os << __FUNCTION__ << ": score and output low-res candidate junction " << junctionIndex << "\n";
#endif
                mjAssembledCandidateSV.junction[junctionIndex] = mjCandidateSV.junction[junctionIndex];
            }
            else
            {
                if (assemblyIndex >= assemblyData.svs.size()) continue;
                const SVCandidate& assembledSV(assemblyData.svs[assemblyIndex]);
#ifdef DEBUG_GSV
            log_os << __FUNCTION__ << ": score and output assembly candidate junction " << junctionIndex << ": " << assembledSV << "\n";
#endif
                mjAssembledCandidateSV.junction[junctionIndex] = assembledSV;
            }
            assemblyIndex++;
            isWrite = true;
        }
        if (! isWrite) break;
        _svWriter.writeSV(edge, svData, mjAssemblyData, mjAssembledCandidateSV);
    }
}