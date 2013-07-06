// -*- mode: c++; indent-tabs-mode: nil; -*-
//
// Manta
// Copyright (c) 2013 Illumina, Inc.
//
// This software is provided under the terms and conditions of the
// Illumina Open Source Software License 1.
//
// You should have received a copy of the Illumina Open Source
// Software License 1 along with this program. If not, see
// <https://github.com/downloads/sequencing/licenses/>.
//

///
/// \author Chris Saunders
///

#include "manta/SVLocusScanner.hh"
#include "blt_util/align_path_bam_util.hh"
#include "blt_util/log.hh"

#include "boost/foreach.hpp"



SVLocusScanner::
SVLocusScanner(
    const ReadScannerOptions& opt,
    const std::string& statsFilename,
    const std::vector<std::string>& alignmentFilename) :
    _opt(opt)
{
    // pull in insert stats:
    _rss.read(statsFilename.c_str());

    // cache the insert stats we'll be looking up most often:
    BOOST_FOREACH(const std::string& file, alignmentFilename)
    {
        const boost::optional<unsigned> index(_rss.getGroupIndex(file));
        assert(index);
        const ReadGroupStats rgs(_rss.getStats(*index));

        _stats.resize(_stats.size()+1);
        _stats.back().min=rgs.fragSize.quantile(_opt.breakendEdgeTrimProb);
        _stats.back().max=rgs.fragSize.quantile((1-_opt.breakendEdgeTrimProb));
    }
}



void
SVLocusScanner::
getReadBreakendsImpl(
    const CachedReadGroupStats& rstats,
    const bam_record& localRead,
    const bam_record* remoteReadPtr,
    SVBreakend& localBreakend,
    SVBreakend& remoteBreakend,
    known_pos_range2& evidenceRange)
{
    ALIGNPATH::path_t apath;
    bam_cigar_to_apath(localRead.raw_cigar(),localRead.n_cigar(),apath);

    const unsigned readSize(apath_read_length(apath));
    const unsigned localRefLength(apath_ref_length(apath));

    unsigned thisReadNoninsertSize(0);
    if (localRead.is_fwd_strand())
    {
        thisReadNoninsertSize=(readSize-apath_read_trail_size(apath));
    }
    else
    {
        thisReadNoninsertSize=(readSize-apath_read_lead_size(apath));
    }

    localBreakend.count = 1;

    // if remoteRead is not available, estimate mate localRead size to be same as local,
    // and assume no clipping on mate localRead:
    unsigned remoteReadNoninsertSize(readSize);
    unsigned remoteRefLength(localRefLength);

    if(NULL != remoteReadPtr)
    {
        // if remoteRead is available, we can more accurately determine the size:
        const bam_record& remoteRead(*remoteReadPtr);

        ALIGNPATH::path_t remoteApath;
        bam_cigar_to_apath(remoteRead.raw_cigar(),remoteRead.n_cigar(),remoteApath);

        const unsigned remoteReadSize(apath_read_length(remoteApath));
        remoteRefLength = (apath_ref_length(remoteApath));

        if (remoteRead.is_fwd_strand())
        {
            remoteReadNoninsertSize=(remoteReadSize-apath_read_trail_size(remoteApath));
        }
        else
        {
            remoteReadNoninsertSize=(remoteReadSize-apath_read_lead_size(remoteApath));
        }

        remoteBreakend.count = 1;
    }

    const unsigned totalNoninsertSize(thisReadNoninsertSize+remoteReadNoninsertSize);

    {
        localBreakend.interval.tid = (localRead.target_id());

        const pos_t startRefPos(localRead.pos()-1);
        const pos_t endRefPos(startRefPos+localRefLength);
        // expected breakpoint range is from the end of the localRead alignment to the (probabilistic) end of the fragment:
        if (localRead.is_fwd_strand())
        {
            localBreakend.state = SVBreakendState::RIGHT_OPEN;
            localBreakend.interval.range.set_begin_pos(endRefPos);
            localBreakend.interval.range.set_end_pos(endRefPos + static_cast<pos_t>(rstats.max-(totalNoninsertSize)));
        }
        else
        {
            localBreakend.state = SVBreakendState::LEFT_OPEN;
            localBreakend.interval.range.set_end_pos(startRefPos);
            localBreakend.interval.range.set_begin_pos(startRefPos - static_cast<pos_t>(rstats.max-(totalNoninsertSize)));
        }

        evidenceRange.set_range(startRefPos,endRefPos);
    }

    // get remote breakend estimate:
    {
        remoteBreakend.interval.tid = (localRead.mate_target_id());

        const pos_t startRefPos(localRead.mate_pos()-1);
        pos_t endRefPos(startRefPos+remoteRefLength);
        if (localRead.is_mate_fwd_strand())
        {
            remoteBreakend.state = SVBreakendState::RIGHT_OPEN;
            remoteBreakend.interval.range.set_begin_pos(endRefPos);
            remoteBreakend.interval.range.set_end_pos(endRefPos + static_cast<pos_t>(rstats.max-(totalNoninsertSize)));
        }
        else
        {
            remoteBreakend.state = SVBreakendState::LEFT_OPEN;
            remoteBreakend.interval.range.set_end_pos(startRefPos);
            remoteBreakend.interval.range.set_begin_pos(startRefPos - static_cast<pos_t>(rstats.max-(totalNoninsertSize)));
        }
    }
}



void
SVLocusScanner::
getChimericSVLocusImpl(
    const CachedReadGroupStats& rstats,
    const bam_record& read,
    SVLocus& locus)
{
    SVBreakend localBreakend;
    SVBreakend remoteBreakend;
    known_pos_range2 evidenceRange;
    getReadBreakendsImpl(rstats, read, NULL, localBreakend, remoteBreakend, evidenceRange);

    // set local breakend estimate:
    NodeIndexType localBreakendNode(0);
    {
        localBreakendNode = locus.addNode(localBreakend.interval);
        locus.setNodeEvidence(localBreakendNode,evidenceRange);
    }

    // set remote breakend estimate:
    {
        const NodeIndexType remoteBreakendNode(locus.addRemoteNode(remoteBreakend.interval));
        locus.linkNodes(localBreakendNode,remoteBreakendNode);
    }

}



bool
SVLocusScanner::
isReadFiltered(const bam_record& read) const
{
    if (read.is_filter()) return true;
    if (read.is_dup()) return true;
    if (read.is_secondary()) return true;
    if (read.is_proper_pair()) return true;
    if (read.map_qual() < _opt.minMapq) return true;
    return false;
}



void
SVLocusScanner::
getChimericSVLocus(const bam_record& read,
                   const unsigned defaultReadGroupIndex,
                   SVLocus& locus) const
{
    locus.clear();

    if (read.is_chimeric())
    {
        const CachedReadGroupStats& rstats(_stats[defaultReadGroupIndex]);
        getChimericSVLocusImpl(rstats,read,locus);
    }
}



void
SVLocusScanner::
getBreakendPair(const bam_record& localRead,
                const bam_record* remoteReadPtr,
                const unsigned defaultReadGroupIndex,
                SVBreakend& localBreakend,
                SVBreakend& remoteBreakend) const
{
    const CachedReadGroupStats& rstats(_stats[defaultReadGroupIndex]);
    known_pos_range2 evidenceRange;
    getReadBreakendsImpl(rstats, localRead, remoteReadPtr, localBreakend, remoteBreakend, evidenceRange);
}
