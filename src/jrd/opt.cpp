/*
 *	PROGRAM:	JRD Access Method
 *	MODULE:		opt.cpp
 *	DESCRIPTION:	Optimizer / record selection expression compiler
 *
 * The contents of this file are subject to the Interbase Public
 * License Version 1.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy
 * of the License at http://www.Inprise.com/IPL.html
 *
 * Software distributed under the License is distributed on an
 * "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either express
 * or implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code was created by Inprise Corporation
 * and its predecessors. Portions created by Inprise Corporation are
 * Copyright (C) Inprise Corporation.
 *
 * All Rights Reserved.
 * Contributor(s): ______________________________________.
 * 2002.10.12: Nickolay Samofatov: Fixed problems with wrong results produced by
 *            outer joins
 * 2001.07.28: John Bellardo: Added code to handle rse_skip nodes.
 * 2001.07.17 Claudio Valderrama: Stop crash with indices and recursive calls
 *            of OPT_compile: indicator csb_indices set to zero after used memory is
 *            returned to the free pool.
 * 2001.02.15: Claudio Valderrama: Don't obfuscate the plan output if a selectable
 *             stored procedure doesn't access tables, views or other procedures directly.
 * 2002.10.29 Sean Leyne - Removed obsolete "Netware" port
 * 2002.10.30: Arno Brinkman: Changes made to gen_retrieval, OPT_compile and make_inversion.
 *             Procedure sort_indices added. The changes in gen_retrieval are that now
 *             an index with high field-count has priority to build an index from.
 *             Procedure make_inversion is changed so that it not pick every index
 *             that comes away, this was slow performance with bad selectivity indices
 *             which most are foreign_keys with a reference to a few records.
 * 2002.11.01: Arno Brinkman: Added match_indices for better support of OR handling
 *             in INNER JOIN (gen_join) statements.
 * 2002.12.15: Arno Brinkman: Added find_used_streams, so that inside opt_compile all the
 *             streams are marked active. This causes that more indices can be used for
 *             a retrieval. With this change BUG SF #219525 is solved too.
 */

#include "firebird.h"
#include <stdio.h>
#include <string.h>
#include "../jrd/jrd.h"
#include "../jrd/align.h"
#include "../jrd/val.h"
#include "../jrd/req.h"
#include "../jrd/exe.h"
#include "../jrd/lls.h"
#include "../jrd/ods.h"
#include "../jrd/btr.h"
#include "../jrd/sort.h"
#include "../jrd/rse.h"
#include "../jrd/ini.h"
#include "../jrd/intl.h"
#include "../jrd/Collation.h"
#include "../common/gdsassert.h"
#include "../jrd/btr_proto.h"
#include "../jrd/cch_proto.h"
#include "../jrd/cmp_proto.h"
#include "../jrd/cvt2_proto.h"
#include "../jrd/dpm_proto.h"
#include "../common/dsc_proto.h"
#include "../jrd/err_proto.h"
#include "../jrd/ext_proto.h"
#include "../jrd/intl_proto.h"

#include "../jrd/lck_proto.h"
#include "../jrd/met_proto.h"
#include "../jrd/mov_proto.h"
#include "../jrd/opt_proto.h"
#include "../jrd/par_proto.h"
#include "../yvalve/gds_proto.h"
#include "../jrd/DataTypeUtil.h"
#include "../jrd/KeywordsTable.h"
#include "../jrd/RecordSourceNodes.h"
#include "../jrd/VirtualTable.h"
#include "../jrd/Monitoring.h"
#include "../jrd/TimeZone.h"
#include "../jrd/UserManagement.h"
#include "../common/classes/array.h"
#include "../common/classes/objects_array.h"
#include "../common/os/os_utils.h"
#include "../jrd/recsrc/RecordSource.h"
#include "../jrd/recsrc/Cursor.h"
#include "../jrd/Mapping.h"
#include "../jrd/DbCreators.h"

#include "../jrd/Optimizer.h"
#include "../dsql/BoolNodes.h"
#include "../dsql/ExprNodes.h"
#include "../dsql/StmtNodes.h"
#include "../jrd/ConfigTable.h"

using namespace Jrd;
using namespace Firebird;

#ifdef DEV_BUILD
#define OPT_DEBUG
#endif


namespace
{
	class River;
	typedef HalfStaticArray<River*, OPT_STATIC_ITEMS> RiverList;

	inline void compose(MemoryPool& pool, BoolExprNode** node1, BoolExprNode* node2)
	{
		if (node2)
			*node1 = (*node1) ? FB_NEW_POOL(pool) BinaryBoolNode(pool, blr_and, *node1, node2) : node2;
	}

	struct SortField
	{
		SortField() : stream(INVALID_STREAM), id(0), desc(NULL)
		{}

		SortField(StreamType _stream, ULONG _id, const dsc* _desc)
			: stream(_stream), id(_id), desc(_desc)
		{}

		StreamType stream;
		ULONG id;
		const dsc* desc;
	};

	class River
	{
	public:
		River(CompilerScratch* csb, RecordSource* rsb, RecordSourceNode* node, const StreamList& streams)
			: m_rsb(rsb), m_nodes(csb->csb_pool), m_streams(csb->csb_pool)
		{
			if (node)
				m_nodes.add(node);

			m_streams.assign(streams);
		}

		River(CompilerScratch* csb, RecordSource* rsb, RiverList& rivers)
			: m_rsb(rsb), m_nodes(csb->csb_pool), m_streams(csb->csb_pool)
		{
			for (River** iter = rivers.begin(); iter < rivers.end(); iter++)
			{
				River* const sub_river = *iter;

				const size_t count = m_streams.getCount();
				const size_t delta = sub_river->m_streams.getCount();
				if (count + delta >= MAX_STREAMS)
					ERR_post(Arg::Gds(isc_too_many_contexts));

				m_nodes.join(sub_river->m_nodes);
				m_streams.join(sub_river->m_streams);
			}
		}

		RecordSource* getRecordSource() const
		{
			return m_rsb;
		}

		const StreamList& getStreams() const
		{
			return m_streams;
		}

		void activate(CompilerScratch* csb)
		{
			for (const StreamType* iter = m_streams.begin(); iter < m_streams.end(); iter++)
				csb->csb_rpt[*iter].activate();
		}

		void deactivate(CompilerScratch* csb)
		{
			for (const StreamType* iter = m_streams.begin(); iter < m_streams.end(); iter++)
				csb->csb_rpt[*iter].deactivate();
		}

		bool isReferenced(CompilerScratch* csb, const ExprNode* node) const
		{
			SortedStreamList nodeStreams;
			node->collectStreams(csb, nodeStreams);

			if (!nodeStreams.hasData())
				return false;

			for (const StreamType* iter = nodeStreams.begin(); iter != nodeStreams.end(); ++iter)
			{
				if (!m_streams.exist(*iter))
					return false;
			}

			return true;
		}

		bool isComputable(CompilerScratch* csb) const
		{
			for (RecordSourceNode* const* iter = m_nodes.begin(); iter < m_nodes.end(); iter++)
			{
				if (!(*iter)->computable(csb, INVALID_STREAM, false))
					return false;
			}

			return true;
		}

		RecordSource* applyLocalBoolean(OptimizerBlk* opt)
		{
			fb_assert(m_rsb);

			CompilerScratch* const csb = opt->opt_csb;

			StreamStateHolder stateHolder(csb);
			stateHolder.deactivate();

			activate(csb);

			BoolExprNode* boolean = NULL;

			const OptimizerBlk::opt_conjunct* const opt_end =
				opt->opt_conjuncts.begin() + opt->opt_base_conjuncts;

			for (OptimizerBlk::opt_conjunct* tail = opt->opt_conjuncts.begin();
				tail < opt_end; tail++)
			{
				BoolExprNode* const node = tail->opt_conjunct_node;

				if (!(tail->opt_conjunct_flags & opt_conjunct_used) &&
					!(node->nodFlags & ExprNode::FLAG_RESIDUAL) &&
					node->computable(csb, INVALID_STREAM, false))
				{
					compose(csb->csb_pool, &boolean, node);
					tail->opt_conjunct_flags |= opt_conjunct_used;
				}
			}

			if (boolean)
				m_rsb = FB_NEW_POOL(csb->csb_pool) FilteredStream(csb, m_rsb, boolean);

			return m_rsb;
		}

	protected:
		RecordSource* m_rsb;
		HalfStaticArray<RecordSourceNode*, OPT_STATIC_ITEMS> m_nodes;
		StreamList m_streams;
	};

	class CrossJoin : public River
	{
	public:
		CrossJoin(CompilerScratch* csb, RiverList& rivers)
			: River(csb, NULL, rivers)
		{
			// Save states of the underlying streams and restore them afterwards

			StreamStateHolder stateHolder(csb, m_streams);

			// Generate record source objects

			const FB_SIZE_T riverCount = rivers.getCount();

			if (riverCount == 1)
			{
				River* const sub_river = rivers.pop();
				m_rsb = sub_river->getRecordSource();
			}
			else
			{
				HalfStaticArray<RecordSource*, OPT_STATIC_ITEMS> rsbs(riverCount);

				// Reorder input rivers according to their possible inter-dependencies

				while (rivers.hasData())
				{
					const auto orgCount = rsbs.getCount();

					for (auto& subRiver : rivers)
					{
						const auto subRsb = subRiver->getRecordSource();
						fb_assert(!rsbs.exist(subRsb));

						subRiver->activate(csb);

						if (subRiver->isComputable(csb))
						{
							rsbs.add(subRsb);
							rivers.remove(&subRiver);
							break;
						}

						subRiver->deactivate(csb);
					}

					if (rsbs.getCount() == orgCount)
						break;
				}

				if (rivers.hasData())
				{
					// Ideally, we should never get here. But just in case it happened, handle it.

					fb_assert(false);

					for (auto& subRiver : rivers)
					{
						const auto subRsb = subRiver->getRecordSource();
						fb_assert(!rsbs.exist(subRsb));

						const auto pos = &subRiver - rivers.begin();
						rsbs.insert(pos, subRsb);
					}

					rivers.clear();
				}

				m_rsb = FB_NEW_POOL(csb->csb_pool) NestedLoopJoin(csb, rsbs.getCount(), rsbs.begin());
			}
		}
	};
} // namespace

static bool augment_stack(ValueExprNode*, ValueExprNodeStack&);
static bool augment_stack(BoolExprNode*, BoolExprNodeStack&);
static void check_indices(const CompilerScratch::csb_repeat*);
static void check_sorts(CompilerScratch*, RseNode*);
static void class_mask(USHORT, ValueExprNode**, ULONG*);
static SLONG decompose(thread_db* tdbb, BoolExprNode* boolNode, BoolExprNodeStack& stack,
	CompilerScratch* csb);
static USHORT distribute_equalities(BoolExprNodeStack& org_stack, CompilerScratch* csb,
	USHORT base_count);
static void find_index_relationship_streams(thread_db* tdbb, OptimizerBlk* opt,
	const StreamList& streams, StreamList& dependent_streams, StreamList& free_streams);
static void form_rivers(thread_db* tdbb, OptimizerBlk* opt, const StreamList& streams,
	RiverList& river_list, SortNode** sort_clause, PlanNode* plan_clause);
static bool form_river(thread_db* tdbb, OptimizerBlk* opt, StreamType count, size_t stream_count,
	StreamList& temp, RiverList& river_list, SortNode** sort_clause);
static void gen_join(thread_db* tdbb, OptimizerBlk* opt, const StreamList& streams,
	RiverList& river_list, SortNode** sort_clause, PlanNode* plan_clause);
static RecordSource* gen_outer(thread_db* tdbb, OptimizerBlk* opt, RseNode* rse,
	RiverList& river_list, SortNode** sort_clause);
static RecordSource* gen_residual_boolean(thread_db* tdbb, OptimizerBlk* opt, RecordSource* prior_rsb);
static RecordSource* gen_retrieval(thread_db* tdbb, OptimizerBlk* opt, StreamType stream,
	SortNode** sort_ptr, bool outer_flag, bool inner_flag, BoolExprNode** return_boolean);
static bool gen_equi_join(thread_db*, OptimizerBlk*, RiverList&);
static double get_cardinality(thread_db*, jrd_rel*, const Format*);
static BoolExprNode* make_inference_node(CompilerScratch*, BoolExprNode*, ValueExprNode*, ValueExprNode*);
static bool map_equal(const ValueExprNode*, const ValueExprNode*, const MapNode*);
static void mark_indices(CompilerScratch::csb_repeat* csbTail, SSHORT relationId);
static bool node_equality(const ValueExprNode*, const ValueExprNode*);
static bool node_equality(const BoolExprNode*, const BoolExprNode*);
static ValueExprNode* optimize_like_similar(thread_db*, CompilerScratch*, ComparativeBoolNode*);
static USHORT river_count(USHORT count, ValueExprNode** eq_class);
static bool search_stack(const ValueExprNode*, const ValueExprNodeStack&);
static void set_direction(SortNode*, SortNode*);
static void set_position(const SortNode*, SortNode*, const MapNode*);
static void sort_indices_by_selectivity(CompilerScratch::csb_repeat* csbTail);


// macro definitions

#ifdef OPT_DEBUG
const int DEBUG_PUNT			= 5;
const int DEBUG_RELATIONSHIPS	= 4;
const int DEBUG_ALL				= 3;
const int DEBUG_CANDIDATE		= 2;
const int DEBUG_BEST			= 1;
const int DEBUG_NONE			= 0;

FILE *opt_debug_file = 0;
static int opt_debug_flag = DEBUG_NONE;
#endif

inline void SET_DEP_BIT(ULONG* array, const SLONG bit)
{
	array[bit / BITS_PER_LONG] |= (1L << (bit % BITS_PER_LONG));
}

/*
inline void CLEAR_DEP_BIT(ULONG* array, const SLONG bit)
{
	array[bit / BITS_PER_LONG] &= ~(1L << (bit % BITS_PER_LONG));
}
*/

inline bool TEST_DEP_BIT(const ULONG* array, const ULONG bit)
{
	return (array[bit / BITS_PER_LONG] & (1L << (bit % BITS_PER_LONG))) != 0;
}

/*
inline bool TEST_DEP_ARRAYS(const ULONG* ar1, const ULONG* ar2)
{
	//return (ar1[0] & ar2[0]) || (ar1[1] & ar2[1]) || (ar1[2] & ar2[2]) || (ar1[3] & ar2[3]) ||
	//	   (ar1[4] & ar2[4]) || (ar1[5] & ar2[5]) || (ar1[6] & ar2[6]) || (ar1[7] & ar2[7]);
	for (SLONG i = 0; i < BITS_PER_LONG; i++)
	{
		if (ar1[i] & ar2[i])
			return true;
	}
	return false;
}
*/

const int CACHE_PAGES_PER_STREAM			= 15;

// enumeration of sort datatypes

static const UCHAR sort_dtypes[] =
{
	0,							// dtype_unknown
	SKD_text,					// dtype_text
	SKD_cstring,				// dtype_cstring
	SKD_varying,				// dtype_varying
	0,
	0,
	0,							// dtype_packed
	0,							// dtype_byte
	SKD_short,					// dtype_short
	SKD_long,					// dtype_long
	SKD_quad,					// dtype_quad
	SKD_float,					// dtype_real
	SKD_double,					// dtype_double
	SKD_double,					// dtype_d_float
	SKD_sql_date,				// dtype_sql_date
	SKD_sql_time,				// dtype_sql_time
	SKD_timestamp,				// dtype_timestamp
	SKD_quad,					// dtype_blob
	0,							// dtype_array
	SKD_int64,					// dtype_int64
	SKD_text,					// dtype_dbkey - use text sort for backward compatibility
	SKD_bytes,					// dtype_boolean
	SKD_dec64,					// dtype_dec64
	SKD_dec128,					// dtype_dec128
	SKD_int128,					// dtype_int128
	SKD_sql_time_tz,			// dtype_sql_time_tz
	SKD_timestamp_tz			// dtype_timestamp_tz
};


string OPT_get_plan(thread_db* tdbb, const jrd_req* request, bool detailed)
{
/**************************************
 *
 *	O P T _ g e t _ p l a n
 *
 **************************************
 *
 * Functional description
 *	Returns a formatted textual plan for all RseNode's in the specified request.
 *
 **************************************/
	string plan;

	if (request)
	{
		const Array<const RecordSource*>& fors = request->getStatement()->fors;

		for (FB_SIZE_T i = 0; i < fors.getCount(); i++)
		{
			plan += detailed ? "\nSelect Expression" : "\nPLAN ";
			fors[i]->print(tdbb, plan, detailed, 0);
		}
	}

	return plan;
}


// Compile and optimize a record selection expression into a set of record source blocks (rsb's).
RecordSource* OPT_compile(thread_db* tdbb, CompilerScratch* csb, RseNode* rse,
	BoolExprNodeStack* parent_stack)
{
	DEV_BLKCHK(csb, type_csb);
	DEV_BLKCHK(rse, type_nod);

	SET_TDBB(tdbb);

#ifdef OPT_DEBUG
	if (opt_debug_flag != DEBUG_NONE && !opt_debug_file)
		opt_debug_file = os_utils::fopen(OPTIMIZER_DEBUG_FILE, "w");
#endif

	// If there is a boolean, there is some work to be done.  First,
	// decompose the boolean into conjunctions.  Then get descriptions
	// of all indices for all relations in the RseNode.  This will give
	// us the info necessary to allocate a optimizer block big
	// enough to hold this crud.

	// Do not allocate the index_desc struct. Let BTR_all do the job. The allocated
	// memory will then be in csb->csb_rpt[stream].csb_idx_allocation, which
	// gets cleaned up before this function exits.

	MemoryPool* const pool = tdbb->getDefaultPool();

	AutoPtr<OptimizerBlk> opt(FB_NEW_POOL(*pool) OptimizerBlk(pool, rse));
	opt->opt_streams.grow(csb->csb_n_stream);
	opt->favorFirstRows = (rse->flags & RseNode::FLAG_OPT_FIRST_ROWS) != 0;

	RecordSource* rsb = NULL;

	try {

	opt->opt_csb = csb;

	RiverList rivers;

	check_sorts(csb, rse);
	SortNode* sort = rse->rse_sorted;
	SortNode* project = rse->rse_projection;
	SortNode* aggregate = rse->rse_aggregate;

	BoolExprNodeStack conjunct_stack;
	SLONG conjunct_count = 0;

	// put any additional booleans on the conjunct stack, and see if we
	// can generate additional booleans by associativity--this will help
	// to utilize indices that we might not have noticed
	if (rse->rse_boolean)
		conjunct_count = decompose(tdbb, rse->rse_boolean, conjunct_stack, csb);

	conjunct_count += distribute_equalities(conjunct_stack, csb, conjunct_count);

	// AB: If we have limit our retrieval with FIRST / SKIP syntax then
	// we may not deliver above conditions (from higher rse's) to this
	// rse, because the results should be consistent.
    if (rse->rse_skip || rse->rse_first)
		parent_stack = NULL;

	// Set base-point before the parent/distributed nodes begin.
	const USHORT base_count = (USHORT) conjunct_count;
	opt->opt_base_conjuncts = base_count;

	// AB: Add parent conjunctions to conjunct_stack, keep in mind
	// the outer-streams! For outer streams put missing (IS NULL)
	// conjunctions in the missing_stack.
	//
	// opt_rpt[0..opt_base_conjuncts-1] = defined conjunctions to this stream
	// opt_rpt[0..opt_base_parent_conjuncts-1] = defined conjunctions to this
	//   stream and allowed distributed conjunctions (with parent)
	// opt_rpt[0..opt_base_missing_conjuncts-1] = defined conjunctions to this
	//   stream and allowed distributed conjunctions and allowed parent
	// opt_rpt[0..opt_conjuncts_count-1] = all conjunctions
	//
	// allowed = booleans that can never evaluate to NULL/Unknown or turn
	//   NULL/Unknown into a True or False.

	USHORT parent_count = 0, distributed_count = 0;
	BoolExprNodeStack missing_stack;

	if (parent_stack)
	{
		for (BoolExprNodeStack::iterator iter(*parent_stack);
			 iter.hasData() && conjunct_count < MAX_CONJUNCTS; ++iter)
		{
			BoolExprNode* const node = iter.object();

			if (rse->rse_jointype != blr_inner && node->possiblyUnknown(opt))
			{
				// parent missing conjunctions shouldn't be
				// distributed to FULL OUTER JOIN streams at all
				if (rse->rse_jointype != blr_full)
					missing_stack.push(node);
			}
			else
			{
				conjunct_stack.push(node);
				conjunct_count++;
				parent_count++;
			}
		}

		// We've now merged parent, try again to make more conjunctions.
		distributed_count = distribute_equalities(conjunct_stack, csb, conjunct_count);
		conjunct_count += distributed_count;
	}

	// The newly created conjunctions belong to the base conjunctions.
	// After them are starting the parent conjunctions.
	opt->opt_base_parent_conjuncts = opt->opt_base_conjuncts + distributed_count;

	// Set base-point before the parent IS NULL nodes begin
	opt->opt_base_missing_conjuncts = (USHORT) conjunct_count;

	// Check if size of optimizer block exceeded.
	if (conjunct_count > MAX_CONJUNCTS)
	{
		ERR_post(Arg::Gds(isc_optimizer_blk_exc));
		// Msg442: size of optimizer block exceeded
	}

	// Put conjunctions in opt structure.
	// Note that it's a stack and we get the nodes in reversed order from the stack.

	opt->opt_conjuncts.grow(conjunct_count);
	SSHORT nodeBase = -1, j = -1;

	for (SLONG i = conjunct_count; i > 0; i--, j--)
	{
		BoolExprNode* const node = conjunct_stack.pop();

		if (i == base_count)
		{
			// The base conjunctions
			j = base_count - 1;
			nodeBase = 0;
		}
		else if (i == conjunct_count - distributed_count)
		{
			// The parent conjunctions
			j = parent_count - 1;
			nodeBase = opt->opt_base_parent_conjuncts;
		}
		else if (i == conjunct_count)
		{
			// The new conjunctions created by "distribution" from the stack
			j = distributed_count - 1;
			nodeBase = opt->opt_base_conjuncts;
		}

		fb_assert(nodeBase >= 0 && j >= 0 && nodeBase + j < MAX_CONJUNCTS);
		opt->opt_conjuncts[nodeBase + j].opt_conjunct_node = node;
	}

	// Put the parent missing nodes on the stack
	for (BoolExprNodeStack::iterator iter(missing_stack);
		 iter.hasData() && conjunct_count < MAX_CONJUNCTS; ++iter)
	{
		BoolExprNode* const node = iter.object();

		opt->opt_conjuncts.grow(conjunct_count + 1);
		opt->opt_conjuncts[conjunct_count].opt_conjunct_node = node;
		conjunct_count++;
	}

	// clear the csb_active flag of all streams in the RseNode
	StreamList rseStreams;
	rse->computeRseStreams(rseStreams);

	for (StreamList::iterator i = rseStreams.begin(); i != rseStreams.end(); ++i)
		csb->csb_rpt[*i].deactivate();

	// go through the record selection expression generating
	// record source blocks for all streams

	NestConst<RecordSourceNode>* ptr = rse->rse_relations.begin();
	for (NestConst<RecordSourceNode>* const end = rse->rse_relations.end(); ptr != end; ++ptr)
	{
		const bool innerSubStream = (ptr != rse->rse_relations.begin());
		RecordSourceNode* const node = *ptr;

		opt->localStreams.clear();

		fb_assert(sort == rse->rse_sorted);
		fb_assert(aggregate == rse->rse_aggregate);

		// find the stream number and place it at the end of the beds array
		// (if this is really a stream and not another RseNode)

		rsb = node->compile(tdbb, opt, innerSubStream);

		// if an rsb has been generated, we have a non-relation;
		// so it forms a river of its own since it is separately
		// optimized from the streams in this rsb

		if (rsb)
		{
			// AB: Save all outer-part streams
			if (rse->rse_jointype == blr_inner ||
				(rse->rse_jointype == blr_left && !innerSubStream))
			{
				rsb->findUsedStreams(opt->subStreams);
				rsb->findUsedStreams(opt->outerStreams);
			}

			const auto river = FB_NEW_POOL(*pool) River(csb, rsb, node, opt->localStreams);
			river->deactivate(csb);
			rivers.add(river);
		}
		else
		{
			// We have a relation, just add its stream
			fb_assert(opt->beds.hasData());
			opt->outerStreams.add(opt->beds.back());
		}
	}

	// this is an attempt to make sure we have a large enough cache to
	// efficiently retrieve this query; make sure the cache has a minimum
	// number of pages for each stream in the RseNode (the number is just a guess)
	if (opt->compileStreams.getCount() > 5)
		CCH_expand(tdbb, (ULONG) (opt->compileStreams.getCount() * CACHE_PAGES_PER_STREAM));

	// At this point we are ready to start optimizing.
	// We will use the opt block to hold information of
	// a global nature, meaning that it needs to stick
	// around for the rest of the optimization process.

	// attempt to optimize aggregates via an index, if possible
	if (aggregate && !sort)
		sort = aggregate;
	else
		rse->rse_aggregate = aggregate = NULL;

	// AB: Mark the previous used streams (sub-RseNode's) as active
	for (StreamList::iterator i = opt->subStreams.begin(); i != opt->subStreams.end(); ++i)
		csb->csb_rpt[*i].activate();

	bool sortCanBeUsed = true;
	SortNode* const orgSortNode = sort;

	// When DISTINCT and ORDER BY are done on different fields,
	// and ORDER BY can be mapped to an index, then the records
	// are returned in the wrong order because DISTINCT sort is
	// performed after the navigational walk of the index.
	// For that reason, we need to de-optimize this case so that
	// ORDER BY does not use an index.
	if (sort && project)
	{
		sort = NULL;
		sortCanBeUsed = false;
	}

	// outer joins require some extra processing
	if (rse->rse_jointype != blr_inner)
		rsb = gen_outer(tdbb, opt, rse, rivers, &sort);
	else
	{
		// AB: If previous rsb's are already on the stack we can't use
		// a navigational-retrieval for an ORDER BY because the next
		// streams are JOINed to the previous ones
		if (rivers.hasData())
		{
			sort = NULL;
			sortCanBeUsed = false;

			// AB: We could already have multiple rivers at this
			// point so try to do some hashing or sort/merging now.
			while (gen_equi_join(tdbb, opt, rivers))
				;

			// AB: Mark the previous used streams (sub-RseNode's) again
			// as active, because a SORT/MERGE could reset the flags
			for (StreamList::iterator i = opt->subStreams.begin(); i != opt->subStreams.end(); ++i)
				csb->csb_rpt[*i].activate();
		}

		fb_assert(opt->compileStreams.getCount() != 1 || csb->csb_rpt[opt->compileStreams[0]].csb_relation != 0);

		while (true)
		{
			// AB: Determine which streams have an index relationship
			// with the currently active rivers. This is needed so that
			// no merge is made between a new cross river and the
			// currently active rivers. Where in the new cross river
			// a stream depends (index) on the active rivers.
			StreamList dependent_streams, free_streams;
			find_index_relationship_streams(tdbb, opt, opt->compileStreams, dependent_streams, free_streams);

			// If we have dependent and free streams then we can't rely on
			// the sort node to be used for index navigation.
			if (dependent_streams.getCount() && free_streams.getCount())
			{
				sort = NULL;
				sortCanBeUsed = false;
			}

			if (dependent_streams.getCount())
			{
				// copy free streams
				opt->compileStreams.assign(free_streams);

				// Make rivers from the dependent streams
				gen_join(tdbb, opt, dependent_streams, rivers, &sort, rse->rse_plan);

				// Generate one river which holds a cross join rsb between
				// all currently available rivers

				rivers.add(FB_NEW_POOL(*pool) CrossJoin(csb, rivers));
				rivers.back()->activate(csb);
			}
			else
			{
				if (free_streams.getCount())
				{
					// Deactivate streams from rivers on stack, because
					// the remaining streams don't have any indexed relationship with them
					for (River** iter = rivers.begin(); iter < rivers.end(); iter++)
						(*iter)->deactivate(csb);
				}

				break;
			}
		}

		// attempt to form joins in decreasing order of desirability
		gen_join(tdbb, opt, opt->compileStreams, rivers, &sort, rse->rse_plan);

		// If there are multiple rivers, try some hashing or sort/merging
		while (gen_equi_join(tdbb, opt, rivers))
			;

		rsb = CrossJoin(csb, rivers).getRecordSource();

		// Pick up any residual boolean that may have fallen thru the cracks
		rsb = gen_residual_boolean(tdbb, opt, rsb);
	}

	// Assign the sort node back if it wasn't used by the index navigation
	if (orgSortNode && !sortCanBeUsed)
		sort = orgSortNode;

	// if the aggregate was not optimized via an index, get rid of the
	// sort and flag the fact to the calling routine
	if (aggregate && sort)
	{
		rse->rse_aggregate = NULL;
		sort = NULL;
	}

	// check index usage in all the base streams to ensure
	// that any user-specified access plan is followed

	for (StreamType i = 0; i < opt->compileStreams.getCount(); i++)
		check_indices(&csb->csb_rpt[opt->compileStreams[i]]);

	if (project || sort)
	{
		// CVC: I'm not sure how to do this with Array in a clearer way.
		// Please, once you agree with my changes or fix them, you can delete the comments.
		// Eliminate any duplicate dbkey streams
		const StreamType* const b_end = opt->beds.end();
		const StreamType* const k_end = opt->keyStreams.end();
		StreamType* k = opt->keyStreams.begin();
		for (const StreamType* p2 = k; p2 < k_end; ++p2)
		{
			const StreamType* q = opt->beds.begin();

			while (q < b_end && *q != *p2)
				q++;

			if (q >= b_end)
				*k++ = *p2;
		}
		opt->keyStreams.shrink(k - opt->keyStreams.begin());

		// Handle project clause, if present
		if (project)
			rsb = OPT_gen_sort(tdbb, opt->opt_csb, opt->beds, &opt->keyStreams, rsb, project, opt->favorFirstRows, true);

		// Handle sort clause if present
		if (sort)
			rsb = OPT_gen_sort(tdbb, opt->opt_csb, opt->beds, &opt->keyStreams, rsb, sort, opt->favorFirstRows, false);
	}

    // Handle first and/or skip.  The skip MUST (if present)
    // appear in the rsb list AFTER the first.  Since the gen_first and gen_skip
    // functions add their nodes at the beginning of the rsb list we MUST call
    // gen_skip before gen_first.

    if (rse->rse_skip)
		rsb = FB_NEW_POOL(*pool) SkipRowsStream(csb, rsb, rse->rse_skip);

	if (rse->rse_first)
		rsb = FB_NEW_POOL(*pool) FirstRowsStream(csb, rsb, rse->rse_first);

	if (rse->flags & RseNode::FLAG_WRITELOCK)
	{
		for (StreamType i = 0; i < opt->compileStreams.getCount(); ++i)
		{
			const StreamType loopStream = opt->compileStreams[i];
			CompilerScratch::csb_repeat* r = &csb->csb_rpt[loopStream];
			r->csb_flags |= csb_update;

			if (r->csb_relation)
			{
				CMP_post_access(tdbb, csb, r->csb_relation->rel_security_name,
					r->csb_view ? r->csb_view->rel_id : 0,
					SCL_update, SCL_object_table, r->csb_relation->rel_name);
			}
		}
	}

	// release memory allocated for index descriptions
	for (StreamType i = 0; i < opt->compileStreams.getCount(); ++i)
	{
		const StreamType loopStream = opt->compileStreams[i];
		delete csb->csb_rpt[loopStream].csb_idx;
		csb->csb_rpt[loopStream].csb_idx = NULL;

		// CVC: The following line added because OPT_compile is recursive, both directly
		//   and through gen_union(), too. Otherwise, we happen to step on deallocated memory
		//   and this is the cause of the crashes with indices that have plagued IB since v4.

		csb->csb_rpt[loopStream].csb_indices = 0;
	}

#ifdef OPT_DEBUG
	if (opt_debug_file)
	{
		fflush(opt_debug_file);
		//fclose(opt_debug_file);
		//opt_debug_file = 0;
	}
#endif

	}	// try
	catch (const Exception&)
	{
		for (StreamType i = 0; i < opt->compileStreams.getCount(); ++i)
		{
			const StreamType loopStream = opt->compileStreams[i];
			delete csb->csb_rpt[loopStream].csb_idx;
			csb->csb_rpt[loopStream].csb_idx = NULL;
			csb->csb_rpt[loopStream].csb_indices = 0; // Probably needed to be safe
		}

		throw;
	}

	return rsb;
}


// Prepare relation and its indices for optimization.
void OPT_compile_relation(thread_db* tdbb, jrd_rel* relation, CompilerScratch* csb,
						  StreamType stream, bool needIndices)
{
	CompilerScratch::csb_repeat* const tail = &csb->csb_rpt[stream];
	RelationPages* const relPages = relation->getPages(tdbb);

	if (needIndices && !relation->rel_file && !relation->isVirtual())
	{
		tail->csb_indices = BTR_all(tdbb, relation, &tail->csb_idx, relPages);

		if (tail->csb_plan)
			mark_indices(tail, relation->rel_id);
		else
			sort_indices_by_selectivity(tail);
	}
	else
		tail->csb_indices = 0;

	tail->csb_cardinality =
		get_cardinality(tdbb, relation, CMP_format(tdbb, csb, stream));
}

// Add node (ValueExprNode) to stack unless node is already on stack.
static bool augment_stack(ValueExprNode* node, ValueExprNodeStack& stack)
{
/**************************************
 *
 *	a u g m e n t _ s t a c k
 *
 **************************************
 *
 * Functional description
 *
 **************************************/

	for (ValueExprNodeStack::const_iterator temp(stack); temp.hasData(); ++temp)
	{
		if (node_equality(node, temp.object()))
			return false;
	}

	stack.push(node);

	return true;
}

// Add node (BoolExprNode) to stack unless node is already on stack.
static bool augment_stack(BoolExprNode* node, BoolExprNodeStack& stack)
{
	for (BoolExprNodeStack::const_iterator temp(stack); temp.hasData(); ++temp)
	{
		if (node_equality(node, temp.object()))
			return false;
	}

	stack.push(node);

	return true;
}


static void check_indices(const CompilerScratch::csb_repeat* csb_tail)
{
/**************************************
 *
 *	c h e c k _ i n d i c e s
 *
 **************************************
 *
 * Functional description
 *	Check to make sure that the user-specified
 *	indices were actually utilized by the optimizer.
 *
 **************************************/
	thread_db* tdbb = JRD_get_thread_data();

	const PlanNode* plan = csb_tail->csb_plan;
	if (!plan)
		return;

	if (plan->type != PlanNode::TYPE_RETRIEVE)
		return;

	const jrd_rel* relation = csb_tail->csb_relation;

	// if there were no indices fetched at all but the
	// user specified some, error out using the first index specified

	if (!csb_tail->csb_indices && plan->accessType && !tdbb->getAttachment()->isGbak())
	{
		// index %s cannot be used in the specified plan
		ERR_post(Arg::Gds(isc_index_unused) << plan->accessType->items[0].indexName);
	}

	// check to make sure that all indices are either used or marked not to be used,
	// and that there are no unused navigational indices
	MetaName index_name;

	const index_desc* idx = csb_tail->csb_idx->items;

	for (USHORT i = 0; i < csb_tail->csb_indices; i++)
	{
		if (!(idx->idx_runtime_flags & (idx_plan_dont_use | idx_used)) ||
			((idx->idx_runtime_flags & idx_plan_navigate) && !(idx->idx_runtime_flags & idx_navigate)))
		{
			if (relation)
				MET_lookup_index(tdbb, index_name, relation->rel_name, (USHORT) (idx->idx_id + 1));
			else
				index_name = "";

			// index %s cannot be used in the specified plan
			ERR_post(Arg::Gds(isc_index_unused) << Arg::Str(index_name));
		}

		++idx;
	}
}


static void check_sorts(CompilerScratch* csb, RseNode* rse)
{
/**************************************
 *
 *	c h e c k _ s o r t s
 *
 **************************************
 *
 * Functional description
 *	Try to optimize out unnecessary sorting.
 *
 **************************************/
	DEV_BLKCHK(rse, type_nod);

	SortNode* sort = rse->rse_sorted;
	SortNode* project = rse->rse_projection;

	// check if a GROUP BY exists using the same fields as the project or sort:
	// if so, the projection can be eliminated; if no projection exists, then
	// the sort can be eliminated.

	SortNode* group;
	RecordSourceNode* sub_rse;

	if ((project || sort) && rse->rse_relations.getCount() == 1 && (sub_rse = rse->rse_relations[0]) &&
		nodeIs<AggregateSourceNode>(sub_rse) &&
		(group = static_cast<AggregateSourceNode*>(sub_rse)->group))
	{
		MapNode* const map = static_cast<AggregateSourceNode*>(sub_rse)->map;

		// if all the fields of the project are the same as all the fields
		// of the group by, get rid of the project.

		if (project && (project->expressions.getCount() == group->expressions.getCount()))
		{
			NestConst<ValueExprNode>* project_ptr = project->expressions.begin();
			const NestConst<ValueExprNode>* const project_end = project->expressions.end();

			for (; project_ptr != project_end; ++project_ptr)
			{
				const NestConst<ValueExprNode>* group_ptr = group->expressions.begin();
				const NestConst<ValueExprNode>* const group_end = group->expressions.end();

				for (; group_ptr != group_end; ++group_ptr)
				{
					if (map_equal(*group_ptr, *project_ptr, map))
						break;
				}

				if (group_ptr == group_end)
					break;
			}

			// we can now ignore the project, but in case the project is being done
			// in descending order because of an order by, do the group by the same way.
			if (project_ptr == project_end)
			{
				set_direction(project, group);
				project = rse->rse_projection = NULL;
			}
		}

		// if there is no projection, then we can make a similar optimization
		// for sort, except that sort may have fewer fields than group by.

		if (!project && sort && (sort->expressions.getCount() <= group->expressions.getCount()))
		{
			const size_t count = sort->expressions.getCount();
			const NestConst<ValueExprNode>* sort_ptr = sort->expressions.begin();
			const NestConst<ValueExprNode>* const sort_end = sort_ptr + count;

			for (; sort_ptr != sort_end; ++sort_ptr)
			{
				const NestConst<ValueExprNode>* group_ptr = group->expressions.begin();
				const NestConst<ValueExprNode>* const group_end = group_ptr + count;

				for (; group_ptr != group_end; ++group_ptr)
				{
					if (map_equal(*group_ptr, *sort_ptr, map))
						break;
				}

				if (group_ptr == group_end)
					break;
			}

			// if all the fields in the sort list match the first n fields in the
			// project list, we can ignore the sort, but update the sort order
			// (ascending/descending) to match that in the sort list

			if (sort_ptr == sort_end)
			{
				set_direction(sort, group);
				set_position(sort, group, static_cast<AggregateSourceNode*>(sub_rse)->map);
				sort = rse->rse_sorted = NULL;
			}
		}

	}

	// examine the ORDER BY and DISTINCT clauses; if all the fields in the
	// ORDER BY match the first n fields in the DISTINCT in any order, the
	// ORDER BY can be removed, changing the fields in the DISTINCT to match
	// the ordering of fields in the ORDER BY.

	if (sort && project && (sort->expressions.getCount() <= project->expressions.getCount()))
	{
		const size_t count = sort->expressions.getCount();
		const NestConst<ValueExprNode>* sort_ptr = sort->expressions.begin();
		const NestConst<ValueExprNode>* const sort_end = sort_ptr + count;

		for (; sort_ptr != sort_end; ++sort_ptr)
		{
			const NestConst<ValueExprNode>* project_ptr = project->expressions.begin();
			const NestConst<ValueExprNode>* const project_end = project_ptr + count;

			for (; project_ptr != project_end; ++project_ptr)
			{
				const FieldNode* sortField = nodeAs<FieldNode>(*sort_ptr);
				const FieldNode* projectField = nodeAs<FieldNode>(*project_ptr);

				if (sortField && projectField &&
					sortField->fieldStream == projectField->fieldStream &&
					sortField->fieldId == projectField->fieldId)
				{
					break;
				}
			}

			if (project_ptr == project_end)
				break;
		}

		// if all the fields in the sort list match the first n fields
		// in the project list, we can ignore the sort, but update
		// the project to match the sort.
		if (sort_ptr == sort_end)
		{
			set_direction(sort, project);
			set_position(sort, project, NULL);
			sort = rse->rse_sorted = NULL;
		}
	}

	// RP: optimize sort with OUTER JOIN
	// if all the fields in the sort list are from one stream, check the stream is
	// the most outer stream, if true update rse and ignore the sort
	if (sort && !project)
	{
		StreamType sort_stream = 0;
		bool usableSort = true;
		NestConst<ValueExprNode>* sort_ptr = sort->expressions.begin();
		const NestConst<ValueExprNode>* const sort_end = sort->expressions.end();

		for (; sort_ptr != sort_end; ++sort_ptr)
		{
			const FieldNode* sortField;

			if ((sortField = nodeAs<FieldNode>(*sort_ptr)))
			{
				// Get stream for this field at this position.
				const StreamType current_stream = sortField->fieldStream;

				// If this is the first position node, save this stream.
				if (sort_ptr == sort->expressions.begin())
			    	sort_stream = current_stream;
				else if (current_stream != sort_stream)
				{
					// If the current stream is different then the previous stream
					// then we can't use this sort for an indexed order retrieval.
					usableSort = false;
					break;
				}
			}
			else
			{
				// If this is not the first position node, reject this sort.
				// Two expressions cannot be mapped to a single index.
				if (sort_ptr > sort->expressions.begin())
				{
					usableSort = false;
					break;
				}

				// This position doesn't use a simple field, thus we should
				// check the expression internals.
				SortedStreamList streams;
				(*sort_ptr)->collectStreams(csb, streams);

				// We can use this sort only if there's a single stream
				// referenced by the expression.
				if (streams.getCount() == 1)
					sort_stream = streams[0];
				else
				{
					usableSort = false;
					break;
				}
			}
		}

		if (usableSort)
		{
			RseNode* new_rse = NULL;
			RecordSourceNode* node = rse;

			while (node)
			{
				if (nodeIs<RseNode>(node))
				{
					new_rse = static_cast<RseNode*>(node);

					// AB: Don't distribute the sort when a FIRST/SKIP is supplied,
					// because that will affect the behaviour from the deeper RSE.
					// dimitr: the same rule applies to explicit/implicit user-defined sorts.
					if (new_rse != rse &&
						(new_rse->rse_first || new_rse->rse_skip ||
						 new_rse->rse_sorted || new_rse->rse_projection))
					{
						node = NULL;
						break;
					}

					// Walk trough the relations of the RSE and see if a
					// matching stream can be found.
					if (new_rse->rse_jointype == blr_inner)
					{
						if (new_rse->rse_relations.getCount() == 1)
							node = new_rse->rse_relations[0];
						else
						{
							bool sortStreamFound = false;
							for (FB_SIZE_T i = 0; i < new_rse->rse_relations.getCount(); i++)
							{
								RecordSourceNode* subNode = new_rse->rse_relations[i];

								if ((nodeIs<RelationSourceNode>(subNode) || nodeIs<LocalTableSourceNode>(subNode)) &&
									subNode->getStream() == sort_stream &&
									new_rse != rse)
								{
									// We have found the correct stream
									sortStreamFound = true;
									break;
								}
							}

							if (sortStreamFound)
							{
								// Set the sort to the found stream and clear the original sort
								new_rse->rse_sorted = sort;
								sort = rse->rse_sorted = NULL;
							}

							node = NULL;
						}
					}
					else if (new_rse->rse_jointype == blr_left)
						node = new_rse->rse_relations[0];
					else
						node = NULL;
				}
				else
				{
					if ((nodeIs<RelationSourceNode>(node) || nodeIs<LocalTableSourceNode>(node)) &&
						node->getStream() == sort_stream &&
						new_rse && new_rse != rse)
					{
						// We have found the correct stream, thus apply the sort here
						new_rse->rse_sorted = sort;
						sort = rse->rse_sorted = NULL;
					}

					node = NULL;
				}
			}
		}
	}
}


static void class_mask(USHORT count, ValueExprNode** eq_class, ULONG* mask)
{
/**************************************
 *
 *	c l a s s _ m a s k
 *
 **************************************
 *
 * Functional description
 *	Given an sort/merge join equivalence class (vector of node pointers
 *	of representative values for rivers), return a bit mask of rivers
 *	with values.
 *
 **************************************/
	if (count > MAX_CONJUNCTS)
	{
		ERR_post(Arg::Gds(isc_optimizer_blk_exc));
		// Msg442: size of optimizer block exceeded
	}

	for (SLONG i = 0; i < OPT_STREAM_BITS; i++)
		mask[i] = 0;

	for (SLONG i = 0; i < count; i++, eq_class++)
	{
		if (*eq_class)
		{
			SET_DEP_BIT(mask, i);
			DEV_BLKCHK(*eq_class, type_nod);
		}
	}
}


static SLONG decompose(thread_db* tdbb, BoolExprNode* boolNode, BoolExprNodeStack& stack,
	CompilerScratch* csb)
{
/**************************************
 *
 *	d e c o m p o s e
 *
 **************************************
 *
 * Functional description
 *	Decompose a boolean into a stack of conjuctions.
 *
 **************************************/
	DEV_BLKCHK(csb, type_csb);

	BinaryBoolNode* binaryNode = nodeAs<BinaryBoolNode>(boolNode);
	ComparativeBoolNode* cmpNode = nodeAs<ComparativeBoolNode>(boolNode);

	if (binaryNode)
	{
		if (binaryNode->blrOp == blr_and)
		{
			SLONG count = decompose(tdbb, binaryNode->arg1, stack, csb);
			count += decompose(tdbb, binaryNode->arg2, stack, csb);
			return count;
		}
		else if (binaryNode->blrOp == blr_or)
		{
			BoolExprNodeStack or_stack;

			if (decompose(tdbb, binaryNode->arg1, or_stack, csb) >= 2)
			{
				binaryNode->arg1 = or_stack.pop();

				while (or_stack.hasData())
				{
					BinaryBoolNode* newBoolNode = FB_NEW_POOL(csb->csb_pool) BinaryBoolNode(
						csb->csb_pool, blr_and);
					newBoolNode->arg1 = or_stack.pop();
					newBoolNode->arg2 = binaryNode->arg1;

					binaryNode->arg1 = newBoolNode;
				}
			}

			or_stack.clear();

			if (decompose(tdbb, binaryNode->arg2, or_stack, csb) >= 2)
			{
				binaryNode->arg2 = or_stack.pop();

				while (or_stack.hasData())
				{
					BinaryBoolNode* newBoolNode = FB_NEW_POOL(csb->csb_pool) BinaryBoolNode(
						csb->csb_pool, blr_and);
					newBoolNode->arg1 = or_stack.pop();
					newBoolNode->arg2 = binaryNode->arg2;

					binaryNode->arg2 = newBoolNode;
				}
			}
		}
	}
	else if (cmpNode)
	{
		// turn a between into (a greater than or equal) AND (a less than  or equal)

		if (cmpNode->blrOp == blr_between)
		{
			ComparativeBoolNode* newCmpNode = FB_NEW_POOL(csb->csb_pool) ComparativeBoolNode(
				csb->csb_pool, blr_geq);
			newCmpNode->arg1 = cmpNode->arg1;
			newCmpNode->arg2 = cmpNode->arg2;

			stack.push(newCmpNode);

			newCmpNode = FB_NEW_POOL(csb->csb_pool) ComparativeBoolNode(csb->csb_pool, blr_leq);
			newCmpNode->arg1 = CMP_clone_node_opt(tdbb, csb, cmpNode->arg1);
			newCmpNode->arg2 = cmpNode->arg3;

			stack.push(newCmpNode);

			return 2;
		}

		// turn a LIKE/SIMILAR into a LIKE/SIMILAR and a STARTING WITH, if it starts
		// with anything other than a pattern-matching character

		ValueExprNode* arg;

		if ((cmpNode->blrOp == blr_like || cmpNode->blrOp == blr_similar) &&
			(arg = optimize_like_similar(tdbb, csb, cmpNode)))
		{
			ComparativeBoolNode* newCmpNode = FB_NEW_POOL(csb->csb_pool) ComparativeBoolNode(
				csb->csb_pool, blr_starting);
			newCmpNode->arg1 = cmpNode->arg1;
			newCmpNode->arg2 = arg;

			stack.push(newCmpNode);
			stack.push(boolNode);

			return 2;
		}
	}

	stack.push(boolNode);

	return 1;
}


static USHORT distribute_equalities(BoolExprNodeStack& org_stack, CompilerScratch* csb,
	USHORT base_count)
{
/**************************************
 *
 *	d i s t r i b u t e _ e q u a l i t i e s
 *
 **************************************
 *
 * Functional description
 *	Given a stack of conjunctions, generate some simple
 *	inferences.  In general, find classes of equalities,
 *	then find operations based on members of those classes.
 *	If we find any, generate additional conjunctions.  In
 *	short:
 *
 *		If (a == b) and (a $ c) --> (b $ c) for any
 *		operation '$'.
 *
 **************************************/

	// dimitr:	Dumb protection against too many injected conjuncts (see CORE-5381).
	//			Don't produce more additional conjuncts than we originally had
	//			(i.e. this routine should never more than double the number of conjuncts).
	//			Ideally, we need two separate limits here:
	//				1) number of injected conjuncts (affects required impure size)
	//				2) number of input conjuncts (affects search time inside this routine)

	if (base_count * 2 > MAX_CONJUNCTS)
		return 0;

	ObjectsArray<ValueExprNodeStack> classes;
	ObjectsArray<ValueExprNodeStack>::iterator eq_class;

	DEV_BLKCHK(csb, type_csb);

	// Zip thru stack of booleans looking for field equalities

	for (BoolExprNodeStack::iterator iter(org_stack); iter.hasData(); ++iter)
	{
		BoolExprNode* const boolean = iter.object();

		if (boolean->nodFlags & ExprNode::FLAG_DEOPTIMIZE)
			continue;

		ComparativeBoolNode* const cmpNode = nodeAs<ComparativeBoolNode>(boolean);

		if (!cmpNode || cmpNode->blrOp != blr_eql)
			continue;

		ValueExprNode* const node1 = cmpNode->arg1;
		if (!nodeIs<FieldNode>(node1))
			continue;

		ValueExprNode* const node2 = cmpNode->arg2;
		if (!nodeIs<FieldNode>(node2))
			continue;

		for (eq_class = classes.begin(); eq_class != classes.end(); ++eq_class)
		{
			if (search_stack(node1, *eq_class))
			{
				augment_stack(node2, *eq_class);
				break;
			}
			else if (search_stack(node2, *eq_class))
			{
				eq_class->push(node1);
				break;
			}
		}

		if (eq_class == classes.end())
		{
			ValueExprNodeStack& s = classes.add();
			s.push(node1);
			s.push(node2);
			eq_class = classes.back();
		}
	}

	if (classes.isEmpty())
		return 0;

	// Make another pass looking for any equality relationships that may have crept
	// in between classes (this could result from the sequence (A = B, C = D, B = C)

	for (eq_class = classes.begin(); eq_class != classes.end(); ++eq_class)
	{
		for (ValueExprNodeStack::const_iterator iter(*eq_class); iter.hasData(); ++iter)
		{
			for (ObjectsArray<ValueExprNodeStack>::iterator eq_class2(eq_class);
				 ++eq_class2 != classes.end();)
			{
				if (search_stack(iter.object(), *eq_class2))
				{
					while (eq_class2->hasData())
						augment_stack(eq_class2->pop(), *eq_class);
				}
			}
		}
	}

	USHORT count = 0;

	// Start by making a pass distributing field equalities

	for (eq_class = classes.begin(); eq_class != classes.end(); ++eq_class)
	{
		if (eq_class->hasMore(2))
		{
			for (ValueExprNodeStack::iterator outer(*eq_class); outer.hasData(); ++outer)
			{
				for (ValueExprNodeStack::iterator inner(outer); (++inner).hasData(); )
				{
					if (count < base_count)
					{
						AutoPtr<ComparativeBoolNode> cmpNode(FB_NEW_POOL(csb->csb_pool)
							ComparativeBoolNode(csb->csb_pool, blr_eql));
						cmpNode->arg1 = outer.object();
						cmpNode->arg2 = inner.object();

						if (augment_stack(cmpNode, org_stack))
						{
							count++;
							cmpNode.release();
						}
					}
				}
			}
		}
	}

	// Now make a second pass looking for non-field equalities

	for (BoolExprNodeStack::iterator iter(org_stack); iter.hasData(); ++iter)
	{
		BoolExprNode* const boolean = iter.object();
		ComparativeBoolNode* const cmpNode = nodeAs<ComparativeBoolNode>(boolean);
		ValueExprNode* node1;
		ValueExprNode* node2;

		if (cmpNode &&
			(cmpNode->blrOp == blr_eql || cmpNode->blrOp == blr_gtr || cmpNode->blrOp == blr_geq ||
			 cmpNode->blrOp == blr_leq || cmpNode->blrOp == blr_lss ||
			 cmpNode->blrOp == blr_matching || cmpNode->blrOp == blr_containing ||
			 cmpNode->blrOp == blr_like || cmpNode->blrOp == blr_similar))
		{
			node1 = cmpNode->arg1;
			node2 = cmpNode->arg2;
		}
		else
			continue;

		bool reverse = false;

		if (!nodeIs<FieldNode>(node1))
		{
			ValueExprNode* swap_node = node1;
			node1 = node2;
			node2 = swap_node;
			reverse = true;
		}

		if (!nodeIs<FieldNode>(node1))
			continue;

		if (!nodeIs<LiteralNode>(node2) && !nodeIs<ParameterNode>(node2) && !nodeIs<VariableNode>(node2))
			continue;

		for (eq_class = classes.begin(); eq_class != classes.end(); ++eq_class)
		{
			if (search_stack(node1, *eq_class))
			{
				for (ValueExprNodeStack::iterator temp(*eq_class); temp.hasData(); ++temp)
				{
					if (!node_equality(node1, temp.object()) && count < base_count)
					{
						ValueExprNode* arg1;
						ValueExprNode* arg2;

						if (reverse)
						{
							arg1 = cmpNode->arg1;
							arg2 = temp.object();
						}
						else
						{
							arg1 = temp.object();
							arg2 = cmpNode->arg2;
						}

						// From the conjuncts X(A,B) and A=C, infer the conjunct X(C,B)
						AutoPtr<BoolExprNode> newNode(make_inference_node(csb, boolean, arg1, arg2));

						if (augment_stack(newNode, org_stack))
						{
							++count;
							newNode.release();
						}
					}
				}

				break;
			}
		}
	}

	return count;
}


static void find_index_relationship_streams(thread_db* tdbb,
											OptimizerBlk* opt,
											const StreamList& streams,
											StreamList& dependent_streams,
											StreamList& free_streams)
{
/**************************************
 *
 *	f i n d _ i n d e x _ r e l a t i o n s h i p _ s t r e a m s
 *
 **************************************
 *
 * Functional description
 *	Find the streams that can use an index
 *	with the currently active streams.
 *
 **************************************/

	DEV_BLKCHK(opt, type_opt);
	SET_TDBB(tdbb);

	CompilerScratch* const csb = opt->opt_csb;
	const StreamType* end_stream = streams.end();
	for (const StreamType* stream = streams.begin(); stream < end_stream; stream++)
	{
		CompilerScratch::csb_repeat* const csb_tail = &csb->csb_rpt[*stream];

		// Set temporary active flag for this stream
		csb_tail->activate();

		bool indexed_relationship = false;

		if (opt->opt_conjuncts.getCount())
		{
			// Calculate the inversion for this stream.
			// The returning candidate contains the streams that will be used for
			// index retrieval. This meant that if some stream is used this stream
			// depends on already active streams and can not be used in a separate
			// SORT/MERGE.

			OptimizerRetrieval optimizerRetrieval(*tdbb->getDefaultPool(), opt, *stream,
												  false, false, NULL);
			AutoPtr<InversionCandidate> candidate(optimizerRetrieval.getCost());

			if (candidate->dependentFromStreams.hasData())
			{
				indexed_relationship = true;
			}
		}

		if (indexed_relationship)
		{
			dependent_streams.add(*stream);
		}
		else
		{
			free_streams.add(*stream);
		}

		// Reset active flag
		csb_tail->deactivate();
	}
}


static void form_rivers(thread_db*		tdbb,
						OptimizerBlk*	opt,
						const StreamList&	streams,
						RiverList&		river_list,
						SortNode**		sort_clause,
						PlanNode*		plan_clause)
{
/**************************************
 *
 *	f o r m _ r i v e r s
 *
 **************************************
 *
 * Functional description
 *	Form streams into rivers according
 *	to the user-specified plan.
 *
 **************************************/
	SET_TDBB(tdbb);
	DEV_BLKCHK(opt, type_opt);

	StreamList temp;

	// this must be a join or a merge node, so go through
	// the substreams and place them into the temp vector
	// for formation into a river.
	PlanNode* plan_node = NULL;
	NestConst<PlanNode>* ptr = plan_clause->subNodes.begin();

	for (const NestConst<PlanNode>* const end = plan_clause->subNodes.end(); ptr != end; ++ptr)
	{
		plan_node = *ptr;

		if (plan_node->type == PlanNode::TYPE_JOIN)
		{
			form_rivers(tdbb, opt, streams, river_list, sort_clause, plan_node);
			continue;
		}

		// at this point we must have a retrieval node, so put
		// the stream into the river.
		fb_assert(plan_node->type == PlanNode::TYPE_RETRIEVE);

		const StreamType stream = plan_node->relationNode->getStream();

		// dimitr:	the plan may contain more retrievals than the "streams"
		//			array (some streams could already be joined to the active
		//			rivers), so we populate the "temp" array only with the
		//			streams that appear in both the plan and the "streams"
		//			array.

		const StreamType* ptr_stream = streams.begin();
		const StreamType* const end_stream = streams.end();

		while (ptr_stream < end_stream)
		{
			if (*ptr_stream++ == stream)
			{
				temp.add(stream);
				break;
			}
		}
	}

	// just because the user specified a join does not mean that
	// we are able to form a river;  thus form as many rivers out
	// of the join are as necessary to exhaust the streams.
	// AB: Only form rivers when any retrieval node is seen, for
	// example a MERGE on two JOINs will come with no retrievals
	// at this point.
	// CVC: Notice "plan_node" is pointing to the last element in the loop above.
	// If the loop didn't execute, we had garbage in "plan_node".

	if (temp.getCount() != 0)
	{
		OptimizerInnerJoin innerJoin(*tdbb->getDefaultPool(), opt, temp,
									 (sort_clause ? *sort_clause : NULL), plan_clause);

		StreamType count;
		do {
			count = innerJoin.findJoinOrder();
		} while (form_river(tdbb, opt, count, streams.getCount(), temp, river_list, sort_clause));
	}
}


static bool form_river(thread_db*		tdbb,
					   OptimizerBlk*	opt,
					   StreamType		count,
					   size_t			stream_count,
					   StreamList&		temp,
					   RiverList&		river_list,
					   SortNode** sort_clause)
{
/**************************************
 *
 *	f o r m _ r i v e r
 *
 **************************************
 *
 * Functional description
 *	Form streams into rivers (combinations of streams).
 *
 **************************************/
	fb_assert(count);

	DEV_BLKCHK(opt, type_opt);
	DEV_BLKCHK(plan_clause, type_nod);

	SET_TDBB(tdbb);

	CompilerScratch* const csb = opt->opt_csb;

	HalfStaticArray<RecordSource*, OPT_STATIC_ITEMS> rsbs;
	rsbs.resize(count);
	RecordSource** ptr = rsbs.begin();

	StreamList streams;
	streams.resize(count);
	StreamType* stream = streams.begin();

	if (count != stream_count)
		sort_clause = NULL;

	const OptimizerBlk::opt_stream* const opt_end = opt->opt_streams.begin() + count;
	for (OptimizerBlk::opt_stream* tail = opt->opt_streams.begin();
		 tail < opt_end; tail++, stream++, ptr++)
	{
		*stream = tail->opt_best_stream;
		*ptr = gen_retrieval(tdbb, opt, *stream, sort_clause, false, false, NULL);
		sort_clause = NULL;
	}

	RecordSource* const rsb = (count == 1) ? rsbs[0] :
		FB_NEW_POOL(*tdbb->getDefaultPool()) NestedLoopJoin(csb, count, rsbs.begin());

	// Allocate a river block and move the best order into it
	River* const river = FB_NEW_POOL(*tdbb->getDefaultPool()) River(csb, rsb, NULL, streams);
	river->deactivate(csb);
	river_list.push(river);

	stream = temp.begin();
	const StreamType* const end_stream = temp.end();

	fb_assert(temp.getCount() >= count);
	temp.shrink(temp.getCount() - count);
	if (!temp.getCount())
		return false;

	// Reform "temp" from streams not consumed
	for (const StreamType* t2 = stream; t2 < end_stream; t2++)
	{
		bool used = false;

		for (OptimizerBlk::opt_stream* tail = opt->opt_streams.begin(); tail < opt_end; tail++)
		{
			if (*t2 == tail->opt_best_stream)
			{
				used = true;
				break;
			}
		}

		if (!used)
			*stream++ = *t2;
	}

	return true;
}


// Generate a separate AggregateSort (Aggregate SortedStream Block) for each distinct operation.
// Note that this should be optimized to use indices if possible.
void OPT_gen_aggregate_distincts(thread_db* tdbb, CompilerScratch* csb, MapNode* map)
{
	DSC descriptor;
	DSC* desc = &descriptor;
	NestConst<ValueExprNode>* ptr = map->sourceList.begin();

	for (const NestConst<ValueExprNode>* const end = map->sourceList.end(); ptr != end; ++ptr)
	{
		ValueExprNode* from = *ptr;
		AggNode* aggNode = nodeAs<AggNode>(from);

		if (aggNode && aggNode->distinct)
		{
			// Build the sort key definition. Turn cstrings into varying text.
			aggNode->arg->getDesc(tdbb, csb, desc);

			if (desc->dsc_dtype == dtype_cstring)
			{
				desc->dsc_dtype = dtype_varying;
				desc->dsc_length++;
			}

			AggregateSort* asb = FB_NEW_POOL(*tdbb->getDefaultPool()) AggregateSort(
				*tdbb->getDefaultPool());
			asb->intl = desc->isText() && desc->getTextType() != ttype_none &&
				desc->getTextType() != ttype_binary && desc->getTextType() != ttype_ascii;

			sort_key_def* sort_key = asb->keyItems.getBuffer(asb->intl ? 2 : 1);
			sort_key->setSkdOffset();

			if (asb->intl)
			{
				const USHORT key_length = ROUNDUP(INTL_key_length(tdbb,
					INTL_TEXT_TO_INDEX(desc->getTextType()), desc->getStringLength()), sizeof(SINT64));

				sort_key->setSkdLength(SKD_bytes, key_length);
				sort_key->skd_flags = SKD_ascending;
				sort_key->skd_vary_offset = 0;

				++sort_key;
				sort_key->setSkdOffset(&sort_key[-1]);
				asb->length = sort_key->getSkdOffset();
			}

			fb_assert(desc->dsc_dtype < FB_NELEM(sort_dtypes));
			sort_key->setSkdLength(sort_dtypes[desc->dsc_dtype], desc->dsc_length);

			if (!sort_key->skd_dtype)
				ERR_post(Arg::Gds(isc_invalid_sort_datatype) << Arg::Str(DSC_dtype_tostring(desc->dsc_dtype)));

			if (desc->dsc_dtype == dtype_varying)
			{
				// allocate space to store varying length
				sort_key->skd_vary_offset = sort_key->getSkdOffset() + ROUNDUP(desc->dsc_length, sizeof(SLONG));
				asb->length = sort_key->skd_vary_offset + sizeof(USHORT);
			}
			else
				asb->length += sort_key->getSkdLength();

			asb->length = ROUNDUP(asb->length, sizeof(SLONG));
			// dimitr:	allocate an extra longword for the purely artificial counter,
			// 			see AggNode::aggPass() for details; the length remains rounded properly
			asb->length += sizeof(ULONG);

			sort_key->skd_flags = SKD_ascending;
			asb->impure = csb->allocImpure<impure_agg_sort>();
			asb->desc = *desc;

			aggNode->asb = asb;
		}
	}
}


static void gen_join(thread_db*		tdbb,
					 OptimizerBlk*	opt,
					 const StreamList&	streams,
					 RiverList&		river_list,
					 SortNode** sort_clause,
					 PlanNode*		plan_clause)
{
/**************************************
 *
 *	g e n _ j o i n
 *
 **************************************
 *
 * Functional description
 *	Find all indexed relationships between streams,
 *	then form streams into rivers (combinations of
 * 	streams).
 *
 **************************************/
	DEV_BLKCHK(opt, type_opt);
	SET_TDBB(tdbb);

	if (!streams.getCount())
		return;

	if (plan_clause && streams.getCount() > 1)
	{
		// this routine expects a join/merge
		form_rivers(tdbb, opt, streams, river_list, sort_clause, plan_clause);
		return;
	}

	OptimizerInnerJoin innerJoin(*tdbb->getDefaultPool(), opt, streams,
								 (sort_clause ? *sort_clause : NULL), plan_clause);

	StreamList temp;
	temp.assign(streams);

	StreamType count;
	do {
		count = innerJoin.findJoinOrder();
	} while (form_river(tdbb, opt, count, streams.getCount(), temp, river_list, sort_clause));
}


static RecordSource* gen_outer(thread_db* tdbb, OptimizerBlk* opt, RseNode* rse,
	RiverList& river_list, SortNode** sort_clause)
{
/**************************************
 *
 *	g e n _ o u t e r
 *
 **************************************
 *
 * Functional description
 *	Generate a top level outer join.  The "outer" and "inner"
 *	sub-streams must be handled differently from each other.
 *	The inner is like other streams.  The outer stream isn't
 *	because conjuncts may not eliminate records from the
 *	stream.  They only determine if a join with an inner
 *	stream record is to be attempted.
 *
 **************************************/
	struct {
		RecordSource* stream_rsb;
		StreamType stream_num;
	} stream_o, stream_i, *stream_ptr[2];

	DEV_BLKCHK(opt, type_opt);
	DEV_BLKCHK(rse, type_nod);
	SET_TDBB(tdbb);

	// Determine which stream should be outer and which is inner.
	// In the case of a left join, the syntactically left stream is the
	// outer, and the right stream is the inner.  For all others, swap
	// the sense of inner and outer, though for a full join it doesn't
	// matter and we should probably try both orders to see which is
	// more efficient.
	if (rse->rse_jointype != blr_left)
	{
		stream_ptr[1] = &stream_o;
		stream_ptr[0] = &stream_i;
	}
	else
	{
		stream_ptr[0] = &stream_o;
		stream_ptr[1] = &stream_i;
	}

	// Loop through the outer join sub-streams in
	// reverse order because rivers may have been PUSHed
	for (int i = 1; i >= 0; i--)
	{
		const RecordSourceNode* node = rse->rse_relations[i];

		if (nodeIs<RelationSourceNode>(node) || nodeIs<LocalTableSourceNode>(node))
		{
			stream_ptr[i]->stream_rsb = NULL;
			stream_ptr[i]->stream_num = node->getStream();
		}
		else
		{
			River* const river = river_list.pop();
			stream_ptr[i]->stream_rsb = river->getRecordSource();
		}
	}

	CompilerScratch* const csb = opt->opt_csb;

	const bool isFullJoin = (rse->rse_jointype == blr_full);

	if (!isFullJoin)
	{
		// Generate rsbs for the sub-streams.
		// For the left sub-stream we also will get a boolean back.
		BoolExprNode* boolean = NULL;

		if (!stream_o.stream_rsb)
		{
			stream_o.stream_rsb = gen_retrieval(tdbb, opt, stream_o.stream_num, sort_clause,
												true, false, &boolean);
		}

		if (!stream_i.stream_rsb)
		{
			// AB: the sort clause for the inner stream of an OUTER JOIN
			//	   should never be used for the index retrieval
			stream_i.stream_rsb =
				gen_retrieval(tdbb, opt, stream_i.stream_num, NULL, false, true, NULL);
		}

		// generate a parent boolean rsb for any remaining booleans that
		// were not satisfied via an index lookup
		stream_i.stream_rsb = gen_residual_boolean(tdbb, opt, stream_i.stream_rsb);

		// Allocate and fill in the rsb
		return FB_NEW_POOL(*tdbb->getDefaultPool())
			NestedLoopJoin(csb, stream_o.stream_rsb, stream_i.stream_rsb,
						   boolean, OUTER_JOIN);
	}

	bool hasOuterRsb = true, hasInnerRsb = true;
	BoolExprNode* boolean = NULL;

	if (!stream_o.stream_rsb)
	{
		hasOuterRsb = false;
		stream_o.stream_rsb =
			gen_retrieval(tdbb, opt, stream_o.stream_num, NULL, true, false, &boolean);
	}

	if (!stream_i.stream_rsb)
	{
		hasInnerRsb = false;
		stream_i.stream_rsb =
			gen_retrieval(tdbb, opt, stream_i.stream_num, NULL, false, true, NULL);
	}

	RecordSource* const innerRsb = gen_residual_boolean(tdbb, opt, stream_i.stream_rsb);

	RecordSource* const rsb1 = FB_NEW_POOL(*tdbb->getDefaultPool())
		NestedLoopJoin(csb, stream_o.stream_rsb, innerRsb, boolean, OUTER_JOIN);

	for (FB_SIZE_T i = 0; i < opt->opt_conjuncts.getCount(); i++)
	{
		if (opt->opt_conjuncts[i].opt_conjunct_flags & opt_conjunct_used)
		{
			BoolExprNode* const org_node = opt->opt_conjuncts[i].opt_conjunct_node;
			opt->opt_conjuncts[i].opt_conjunct_node = CMP_clone_node_opt(tdbb, csb, org_node);
			opt->opt_conjuncts[i].opt_conjunct_flags = 0;
		}
	}

	if (!hasInnerRsb)
		csb->csb_rpt[stream_i.stream_num].deactivate();

	if (!hasOuterRsb)
		csb->csb_rpt[stream_o.stream_num].deactivate();

	boolean = NULL;

	if (!hasInnerRsb)
	{
		stream_i.stream_rsb =
			gen_retrieval(tdbb, opt, stream_i.stream_num, NULL, true, false, &boolean);
	}

	if (!hasOuterRsb)
	{
		stream_o.stream_rsb =
			gen_retrieval(tdbb, opt, stream_o.stream_num, NULL, false, false, NULL);
	}

	RecordSource* const outerRsb = gen_residual_boolean(tdbb, opt, stream_o.stream_rsb);

	RecordSource* const rsb2 = FB_NEW_POOL(*tdbb->getDefaultPool())
		NestedLoopJoin(csb, stream_i.stream_rsb, outerRsb, boolean, ANTI_JOIN);

	return FB_NEW_POOL(*tdbb->getDefaultPool()) FullOuterJoin(csb, rsb1, rsb2);
}


static RecordSource* gen_residual_boolean(thread_db* tdbb, OptimizerBlk* opt, RecordSource* prior_rsb)
{
/**************************************
 *
 *	g e n _ r e s i d u a l _ b o o l e a n
 *
 **************************************
 *
 * Functional description
 *	Pick up any residual boolean remaining,
 *	meaning those that have not been used
 *	as part of some join.  These booleans
 *	must still be applied to the result stream.
 *
 **************************************/
	SET_TDBB(tdbb);
	DEV_BLKCHK(opt, type_opt);
	DEV_BLKCHK(prior_rsb, type_rsb);

	BoolExprNode* boolean = NULL;
	const OptimizerBlk::opt_conjunct* const opt_end =
		opt->opt_conjuncts.begin() + opt->opt_base_conjuncts;

	for (OptimizerBlk::opt_conjunct* tail = opt->opt_conjuncts.begin(); tail < opt_end; tail++)
	{
		BoolExprNode* node = tail->opt_conjunct_node;

		if (!(tail->opt_conjunct_flags & opt_conjunct_used))
		{
			compose(*tdbb->getDefaultPool(), &boolean, node);
			tail->opt_conjunct_flags |= opt_conjunct_used;
		}
	}

	return boolean ?
		FB_NEW_POOL(*tdbb->getDefaultPool()) FilteredStream(opt->opt_csb, prior_rsb, boolean) :
		prior_rsb;
}


static RecordSource* gen_retrieval(thread_db*     tdbb,
						 OptimizerBlk*      opt,
						 StreamType   stream,
						 SortNode** sort_ptr,
						 bool     outer_flag,
						 bool     inner_flag,
						 BoolExprNode** return_boolean)
{
/**************************************
 *
 *	g e n _ r e t r i e v a l
 *
 **************************************
 *
 * Functional description
 *	Compile and optimize a record selection expression into a
 *	set of record source blocks (rsb's).
 *
 **************************************/
	OptimizerBlk::opt_conjunct* tail;

	SET_TDBB(tdbb);

	DEV_BLKCHK(opt, type_opt);

	CompilerScratch* const csb = opt->opt_csb;
	CompilerScratch::csb_repeat* const csb_tail = &csb->csb_rpt[stream];
	jrd_rel* const relation = csb_tail->csb_relation;

	fb_assert(relation);

	const string alias = OPT_make_alias(csb, stream);
	csb_tail->activate();

	// Time to find inversions. For each index on the relation
	// match all unused booleans against the index looking for upper
	// and lower bounds that can be computed by the index. When
	// all unused conjunctions are exhausted, see if there is enough
	// information for an index retrieval. If so, build up an
	// inversion component of the boolean.

	// It's recalculated later.
	const OptimizerBlk::opt_conjunct* opt_end = opt->opt_conjuncts.begin() +
		(inner_flag ? opt->opt_base_missing_conjuncts : opt->opt_conjuncts.getCount());

	RecordSource* rsb = NULL;
	InversionNode* inversion = NULL;
	BoolExprNode* condition = NULL;
	Array<DbKeyRangeNode*> dbkeyRanges;

	if (relation->rel_file)
	{
		// External table
		rsb = FB_NEW_POOL(*tdbb->getDefaultPool()) ExternalTableScan(csb, alias, stream, relation);
	}
	else if (relation->isVirtual())
	{
		// Virtual table: monitoring or security
		switch (relation->rel_id)
		{
		case rel_global_auth_mapping:
			rsb = FB_NEW_POOL(*tdbb->getDefaultPool()) GlobalMappingScan(csb, alias, stream, relation);
			break;

		case rel_sec_users:
		case rel_sec_user_attributes:
			rsb = FB_NEW_POOL(*tdbb->getDefaultPool()) UsersTableScan(csb, alias, stream, relation);
			break;

		case rel_sec_db_creators:
			rsb = FB_NEW_POOL(*tdbb->getDefaultPool()) DbCreatorsScan(csb, alias, stream, relation);
			break;

		case rel_time_zones:
			rsb = FB_NEW_POOL(*tdbb->getDefaultPool()) TimeZonesTableScan(csb, alias, stream, relation);
			break;

		case rel_config:
			rsb = FB_NEW_POOL(*tdbb->getDefaultPool()) ConfigTableScan(csb, alias, stream, relation);
			break;

		case rel_keywords:
			rsb = FB_NEW_POOL(*tdbb->getDefaultPool()) KeywordsTableScan(csb, alias, stream, relation);
			break;

		default:
			rsb = FB_NEW_POOL(*tdbb->getDefaultPool()) MonitoringTableScan(csb, alias, stream, relation);
			break;
		}
	}
	else
	{
		// Persistent table
		OptimizerRetrieval optimizerRetrieval(*tdbb->getDefaultPool(), opt, stream,
											  outer_flag, inner_flag,
											  (sort_ptr ? *sort_ptr : NULL));
		AutoPtr<InversionCandidate> candidate(optimizerRetrieval.getInversion());

		if (candidate)
		{
			inversion = candidate->inversion;
			condition = candidate->condition;
			dbkeyRanges.assign(candidate->dbkeyRanges);

			// Just for safety sake, this condition must be already checked
			// inside OptimizerRetrieval::matchOnIndexes()

			if (inversion && condition &&
				!condition->computable(csb, stream, false))
			{
				fb_assert(false);
				inversion = NULL;
				condition = NULL;
				dbkeyRanges.clear();
			}
		}

		IndexTableScan* const nav_rsb = optimizerRetrieval.getNavigation();

		if (nav_rsb)
		{
			if (sort_ptr)
				*sort_ptr = NULL;

			nav_rsb->setInversion(inversion, condition);

			rsb = nav_rsb;
		}
	}

	if (outer_flag)
	{
		fb_assert(return_boolean);
		// Now make another pass thru the outer conjuncts only, finding unused,
		// computable booleans.  When one is found, roll it into a final
		// boolean and mark it used.
		*return_boolean = NULL;
		opt_end = opt->opt_conjuncts.begin() + opt->opt_base_conjuncts;

		for (tail = opt->opt_conjuncts.begin(); tail < opt_end; tail++)
		{
			BoolExprNode* node = tail->opt_conjunct_node;

			if (!(tail->opt_conjunct_flags & opt_conjunct_used) &&
				!(node->nodFlags & ExprNode::FLAG_RESIDUAL) &&
				node->computable(csb, INVALID_STREAM, false))
			{
				compose(*tdbb->getDefaultPool(), return_boolean, node);
				tail->opt_conjunct_flags |= opt_conjunct_used;
			}
		}
	}

	// Now make another pass thru the conjuncts finding unused, computable
	// booleans.  When one is found, roll it into a final boolean and mark
	// it used. If a computable boolean didn't match against an index then
	// mark the stream to denote unmatched booleans.
	BoolExprNode* boolean = NULL;
	opt_end = opt->opt_conjuncts.begin() + (inner_flag ? opt->opt_base_missing_conjuncts : opt->opt_conjuncts.getCount());
	tail = opt->opt_conjuncts.begin();

	if (outer_flag)
		tail += opt->opt_base_parent_conjuncts;

	for (; tail < opt_end; tail++)
	{
		BoolExprNode* const node = tail->opt_conjunct_node;

		if (!(tail->opt_conjunct_flags & opt_conjunct_used) &&
			!(node->nodFlags & ExprNode::FLAG_RESIDUAL) &&
			node->computable(csb, INVALID_STREAM, false))
		{
			// If inversion is available, utilize all conjuncts that refer to
			// the stream being retrieved. Otherwise, utilize only conjuncts
			// that are local to this stream. The remaining ones are left in piece
			// as possible candidates for a merge/hash join.

			if ((inversion && node->findStream(csb, stream)) ||
				(!inversion && node->computable(csb, stream, true)))
			{
				compose(*tdbb->getDefaultPool(), &boolean, node);
				tail->opt_conjunct_flags |= opt_conjunct_used;

				if (!outer_flag && !(tail->opt_conjunct_flags & opt_conjunct_matched))
					csb_tail->csb_flags |= csb_unmatched;
			}
		}
	}

	if (!rsb)
	{
		if (inversion && condition)
		{
			RecordSource* const rsb1 =
				FB_NEW_POOL(*tdbb->getDefaultPool()) FullTableScan(csb, alias, stream, relation, dbkeyRanges);
			RecordSource* const rsb2 =
				FB_NEW_POOL(*tdbb->getDefaultPool()) BitmapTableScan(csb, alias, stream, relation, inversion);

			rsb = FB_NEW_POOL(*tdbb->getDefaultPool()) ConditionalStream(csb, rsb1, rsb2, condition);
		}
		else if (inversion)
		{
			rsb = FB_NEW_POOL(*tdbb->getDefaultPool()) BitmapTableScan(csb, alias, stream, relation, inversion);
		}
		else
		{
			rsb = FB_NEW_POOL(*tdbb->getDefaultPool()) FullTableScan(csb, alias, stream, relation, dbkeyRanges);

			if (boolean)
				csb->csb_rpt[stream].csb_flags |= csb_unmatched;
		}
	}

	return boolean ? FB_NEW_POOL(*tdbb->getDefaultPool()) FilteredStream(csb, rsb, boolean) : rsb;
}


SortedStream* OPT_gen_sort(thread_db* tdbb, CompilerScratch* csb, const StreamList& streams,
	const StreamList* dbkey_streams, RecordSource* prior_rsb, SortNode* sort,
	bool refetch_flag, bool project_flag)
{
/**************************************
 *
 *	g e n _ s o r t
 *
 **************************************
 *
 * Functional description
 *	Generate a record source block to handle either a sort or a project.
 *	The two case are virtual identical -- the only difference is that
 *	project eliminates duplicates.  However, since duplicates are
 *	recognized and handled by sort, the JRD processing is identical.
 *
 **************************************/
	DEV_BLKCHK(prior_rsb, type_rsb);
	SET_TDBB(tdbb);

	/* We already know the number of keys, but we also need to compute the
	total number of fields, keys and non-keys, to be pumped thru sort.  Starting
	with the number of keys, count the other field referenced.  Since a field
	is often a key, check for overlap to keep the length of the sort record
	down. */

	/* Along with the record number, the transaction id of the
	 * record will also be stored in the sort file.  This will
	 * be used to detect update conflict in read committed
	 * transactions. */

	ULONG items = sort->expressions.getCount() +
		3 * streams.getCount() + 2 * (dbkey_streams ? dbkey_streams->getCount() : 0);
	const NestConst<ValueExprNode>* const end_node = sort->expressions.end();

	// Collect all fields involved into the sort

	HalfStaticArray<SortField, OPT_STATIC_ITEMS> fields;
	ULONG totalLength = 0;

	for (const auto stream : streams)
	{
		UInt32Bitmap::Accessor accessor(csb->csb_rpt[stream].csb_fields);

		if (accessor.getFirst())
		{
			do
			{
				const auto id = accessor.current();

				const auto format = CMP_format(tdbb, csb, stream);
				const auto desc = &format->fmt_desc[id];

				if (id >= format->fmt_count || desc->isUnknown())
					IBERROR(157);		// msg 157 cannot sort on a field that does not exist

				fields.push(SortField(stream, id, desc));
				totalLength += desc->dsc_length;

				// If the field has already been mentioned as a sort key, don't bother to repeat it.
				// Unless this key is computed/volatile and thus cannot be restored after sorting.

				for (auto expr : sort->expressions)
				{
					const auto fieldNode = nodeAs<FieldNode>(expr);

					if (fieldNode && fieldNode->fieldStream == stream && fieldNode->fieldId == id)
					{
						if (!SortedStream::hasVolatileKey(desc))
						{
							totalLength -= desc->dsc_length;
							fields.pop();
						}

						break;
					}
				}

			} while (accessor.getNext());
		}
	}

	auto fieldCount = fields.getCount();

	// Unless refetching is requested explicitly (e.g. FIRST ROWS optimization mode),
	// validate the sort record length against the configured threshold for inline storage

	if (!refetch_flag)
	{
		const auto dbb = tdbb->getDatabase();
		const auto threshold = dbb->dbb_config->getInlineSortThreshold();

		refetch_flag = (totalLength > threshold);
	}

	// Check for persistent fields to be excluded from the sort.
	// If nothing is excluded, there's no point in the refetch mode.

	if (refetch_flag)
	{
		for (auto& item : fields)
		{
			const auto relation = csb->csb_rpt[item.stream].csb_relation;

			if (relation &&
				!relation->rel_file &&
				!relation->rel_view_rse &&
				!relation->isVirtual())
			{
				item.desc = NULL;
				--fieldCount;
			}
		}

		refetch_flag = (fieldCount != fields.getCount());
	}

	items += fieldCount;

	// Now that we know the number of items, allocate a sort map block.
	SortedStream::SortMap* map =
		FB_NEW_POOL(*tdbb->getDefaultPool()) SortedStream::SortMap(*tdbb->getDefaultPool());

	if (project_flag)
		map->flags |= SortedStream::FLAG_PROJECT;

	if (refetch_flag)
		map->flags |= SortedStream::FLAG_REFETCH;

	if (sort->unique)
		map->flags |= SortedStream::FLAG_UNIQUE;

    sort_key_def* prev_key = nullptr;

	// Loop thru sort keys building sort keys.  Actually, to handle null values
	// correctly, two sort keys are made for each field, one for the null flag
	// and one for field itself.

	dsc descriptor;

	SortedStream::SortMap::Item* map_item = map->items.getBuffer(items);
	sort_key_def* sort_key = map->keyItems.getBuffer(2 * sort->expressions.getCount());
	const SortDirection* direction = sort->direction.begin();
	const NullsPlacement* nullOrder = sort->nullOrder.begin();

	for (NestConst<ValueExprNode>* node_ptr = sort->expressions.begin();
		 node_ptr != end_node;
		 ++node_ptr, ++nullOrder, ++direction, ++map_item)
	{
		// Pick up sort key expression.

		NestConst<ValueExprNode> node = *node_ptr;
		dsc* desc = &descriptor;
		node->getDesc(tdbb, csb, desc);

		// Allow for "key" forms of International text to grow
		if (IS_INTL_DATA(desc))
		{
			// Turn varying text and cstrings into text.

			if (desc->dsc_dtype == dtype_varying)
			{
				desc->dsc_dtype = dtype_text;
				desc->dsc_length -= sizeof(USHORT);
			}
			else if (desc->dsc_dtype == dtype_cstring)
			{
				desc->dsc_dtype = dtype_text;
				desc->dsc_length--;
			}

			desc->dsc_length = INTL_key_length(tdbb, INTL_INDEX_TYPE(desc), desc->dsc_length);
		}

		// Make key for null flag
		sort_key->setSkdLength(SKD_text, 1);
		sort_key->setSkdOffset(prev_key);

		// Handle nulls placement
		sort_key->skd_flags = SKD_ascending;

		// Have SQL-compliant nulls ordering for ODS11+
		if ((*nullOrder == NULLS_DEFAULT && *direction != ORDER_DESC) || *nullOrder == NULLS_FIRST)
			sort_key->skd_flags |= SKD_descending;

		prev_key = sort_key++;

		// Make key for sort key proper
		fb_assert(desc->dsc_dtype < FB_NELEM(sort_dtypes));
		sort_key->setSkdLength(sort_dtypes[desc->dsc_dtype], desc->dsc_length);
		sort_key->setSkdOffset(&sort_key[-1], desc);
		sort_key->skd_flags = SKD_ascending;
		if (*direction == ORDER_DESC)
			sort_key->skd_flags |= SKD_descending;

		if (!sort_key->skd_dtype)
			ERR_post(Arg::Gds(isc_invalid_sort_datatype) << Arg::Str(DSC_dtype_tostring(desc->dsc_dtype)));

		if (sort_key->skd_dtype == SKD_varying || sort_key->skd_dtype == SKD_cstring)
		{
			if (desc->dsc_ttype() == ttype_binary)
				sort_key->skd_flags |= SKD_binary;
		}

		if (SortedStream::hasVolatileKey(desc) && !refetch_flag)
			sort_key->skd_flags |= SKD_separate_data;

		map_item->clear();
		map_item->node = node;
		map_item->flagOffset = prev_key->getSkdOffset();
		map_item->desc = *desc;
		map_item->desc.dsc_address = (UCHAR*)(IPTR) sort_key->getSkdOffset();

		prev_key = sort_key++;

		FieldNode* fieldNode;

		if ( (fieldNode = nodeAs<FieldNode>(node)) )
		{
			map_item->stream = fieldNode->fieldStream;
			map_item->fieldId = fieldNode->fieldId;
		}
	}

	fb_assert(prev_key);
	ULONG map_length = prev_key ? ROUNDUP(prev_key->getSkdOffset() + prev_key->getSkdLength(), sizeof(SLONG)) : 0;
	map->keyLength = map_length;
	ULONG flag_offset = map_length;
	map_length += fieldCount;

	// Now go back and process all to fields involved with the sort

	for (const auto& item : fields)
	{
		if (!item.desc)
			continue;

		if (item.desc->dsc_dtype >= dtype_aligned)
			map_length = FB_ALIGN(map_length, type_alignments[item.desc->dsc_dtype]);

		map_item->clear();
		map_item->fieldId = (SSHORT) item.id;
		map_item->stream = item.stream;
		map_item->flagOffset = flag_offset++;
		map_item->desc = *item.desc;
		map_item->desc.dsc_address = (UCHAR*)(IPTR) map_length;
		map_length += item.desc->dsc_length;
		map_item++;
	}

	// Make fields for record numbers and transaction ids for all streams

	map_length = ROUNDUP(map_length, sizeof(SINT64));
	for (const auto stream : streams)
	{
		map_item->clear();
		map_item->fieldId = SortedStream::ID_DBKEY;
		map_item->stream = stream;
		dsc* desc = &map_item->desc;
		desc->dsc_dtype = dtype_int64;
		desc->dsc_length = sizeof(SINT64);
		desc->dsc_address = (UCHAR*)(IPTR) map_length;
		map_length += desc->dsc_length;
		map_item++;

		map_item->clear();
		map_item->fieldId = SortedStream::ID_TRANS;
		map_item->stream = stream;
		desc = &map_item->desc;
		desc->dsc_dtype = dtype_int64;
		desc->dsc_length = sizeof(SINT64);
		desc->dsc_address = (UCHAR*)(IPTR) map_length;
		map_length += desc->dsc_length;
		map_item++;
	}

	if (dbkey_streams && dbkey_streams->hasData())
	{
		map_length = ROUNDUP(map_length, sizeof(SINT64));

		for (const auto stream : *dbkey_streams)
		{
			map_item->clear();
			map_item->fieldId = SortedStream::ID_DBKEY;
			map_item->stream = stream;
			dsc* desc = &map_item->desc;
			desc->dsc_dtype = dtype_int64;
			desc->dsc_length = sizeof(SINT64);
			desc->dsc_address = (UCHAR*)(IPTR) map_length;
			map_length += desc->dsc_length;
			map_item++;
		}

		for (const auto stream : *dbkey_streams)
		{
			map_item->clear();
			map_item->fieldId = SortedStream::ID_DBKEY_VALID;
			map_item->stream = stream;
			dsc* desc = &map_item->desc;
			desc->dsc_dtype = dtype_text;
			desc->dsc_ttype() = CS_BINARY;
			desc->dsc_length = 1;
			desc->dsc_address = (UCHAR*)(IPTR) map_length;
			map_length += desc->dsc_length;
			map_item++;
		}
	}

	for (const auto stream : streams)
	{
		map_item->clear();
		map_item->fieldId = SortedStream::ID_DBKEY_VALID;
		map_item->stream = stream;
		dsc* desc = &map_item->desc;
		desc->dsc_dtype = dtype_text;
		desc->dsc_ttype() = CS_BINARY;
		desc->dsc_length = 1;
		desc->dsc_address = (UCHAR*)(IPTR) map_length;
		map_length += desc->dsc_length;
		map_item++;
	}

	fb_assert(map_item == map->items.end());
	fb_assert(sort_key == map->keyItems.end());

	map_length = ROUNDUP(map_length, sizeof(SLONG));

	// Make fields to store varying and cstring length

	for (auto& sortKey : map->keyItems)
	{
		fb_assert(sortKey.skd_dtype != 0);

		if (sortKey.skd_dtype == SKD_varying || sortKey.skd_dtype == SKD_cstring)
		{
			sortKey.skd_vary_offset = map_length;
			map_length += sizeof(USHORT);
			map->flags |= SortedStream::FLAG_KEY_VARY;
		}
	}

	if (map_length > MAX_SORT_RECORD)
	{
		ERR_post(Arg::Gds(isc_sort_rec_size_err) << Arg::Num(map_length));
		// Msg438: sort record size of %ld bytes is too big
	}

	map->length = map_length;

	// That was most unpleasant.  Never the less, it's done (except for the debugging).
	// All that remains is to build the record source block for the sort.
	return FB_NEW_POOL(*tdbb->getDefaultPool()) SortedStream(csb, prior_rsb, map);
}


static bool gen_equi_join(thread_db* tdbb, OptimizerBlk* opt, RiverList& org_rivers)
{
/**************************************
 *
 *	g e n _ e q u i _ j o i n
 *
 **************************************
 *
 * Functional description
 *	We've got a set of rivers that may or may not be amenable to
 *	a hash join or a sort/merge join, and it's time to find out.
 *	If there are, build an appropriate join RecordSource,
 *	push it on the rsb stack, and update rivers accordingly.
 *	If two or more rivers were successfully joined, return true.
 *	If the whole things is a moby no-op, return false.
 *
 **************************************/
	ULONG selected_rivers[OPT_STREAM_BITS], selected_rivers2[OPT_STREAM_BITS];
	ValueExprNode** eq_class;
	DEV_BLKCHK(opt, type_opt);
	SET_TDBB(tdbb);

	CompilerScratch* const csb = opt->opt_csb;

	// Count the number of "rivers" involved in the operation, then allocate
	// a scratch block large enough to hold values to compute equality
	// classes.

	const USHORT cnt = (USHORT) org_rivers.getCount();

	if (cnt < 2)
		return false;

	HalfStaticArray<ValueExprNode*, OPT_STATIC_ITEMS> scratch;
	scratch.grow(opt->opt_base_conjuncts * cnt);
	ValueExprNode** classes = scratch.begin();

	// Compute equivalence classes among streams. This involves finding groups
	// of streams joined by field equalities.

	ValueExprNode** last_class = classes;
	OptimizerBlk::opt_conjunct* tail = opt->opt_conjuncts.begin();
	const OptimizerBlk::opt_conjunct* const end = tail + opt->opt_base_conjuncts;

	for (; tail < end; tail++)
	{
		if (tail->opt_conjunct_flags & opt_conjunct_used)
			continue;

		BoolExprNode* const node = tail->opt_conjunct_node;
		ComparativeBoolNode* cmpNode = nodeAs<ComparativeBoolNode>(node);

		if (!cmpNode || (cmpNode->blrOp != blr_eql && cmpNode->blrOp != blr_equiv))
			continue;

		ValueExprNode* node1 = cmpNode->arg1;
		ValueExprNode* node2 = cmpNode->arg2;

		dsc result, desc1, desc2;
		node1->getDesc(tdbb, csb, &desc1);
		node2->getDesc(tdbb, csb, &desc2);

		// Ensure that arguments can be compared in the binary form
		if (!CVT2_get_binary_comparable_desc(&result, &desc1, &desc2))
			continue;

		// Cast the arguments, if required
		if (!DSC_EQUIV(&result, &desc1, true) || !DSC_EQUIV(&result, &desc2, true))
		{
			if (!DSC_EQUIV(&result, &desc1, true))
			{
				CastNode* cast = FB_NEW_POOL(*tdbb->getDefaultPool()) CastNode(*tdbb->getDefaultPool());
				cast->source = node1;
				cast->castDesc = result;
				cast->impureOffset = csb->allocImpure<impure_value>();
				node1 = cast;
			}

			if (!DSC_EQUIV(&result, &desc2, true))
			{
				CastNode* cast = FB_NEW_POOL(*tdbb->getDefaultPool()) CastNode(*tdbb->getDefaultPool());
				cast->source = node2;
				cast->castDesc = result;
				cast->impureOffset = csb->allocImpure<impure_value>();
				node2 = cast;
			}
		}

		USHORT number1 = 0;

		for (River** iter1 = org_rivers.begin(); iter1 < org_rivers.end(); iter1++, number1++)
		{
			River* const river1 = *iter1;

			if (!river1->isReferenced(csb, node1))
			{
				if (!river1->isReferenced(csb, node2))
					continue;

				ValueExprNode* const temp = node1;
				node1 = node2;
				node2 = temp;
			}

			USHORT number2 = number1 + 1;

			for (River** iter2 = iter1 + 1; iter2 < org_rivers.end(); iter2++, number2++)
			{
				River* const river2 = *iter2;

				if (river2->isReferenced(csb, node2))
				{
					for (eq_class = classes; eq_class < last_class; eq_class += cnt)
					{
						if (node_equality(node1, classes[number1]) ||
							node_equality(node2, classes[number2]))
						{
							break;
						}
					}

					eq_class[number1] = node1;
					eq_class[number2] = node2;

					if (eq_class == last_class)
						last_class += cnt;
				}
			}
		}
	}

	// Pick both a set of classes and a set of rivers on which to join.
	// Obviously, if the set of classes is empty, return false
	// to indicate that nothing could be done.

	USHORT river_cnt = 0;
	HalfStaticArray<ValueExprNode**, OPT_STATIC_ITEMS> selected_classes(cnt);

	for (eq_class = classes; eq_class < last_class; eq_class += cnt)
	{
		USHORT i = river_count(cnt, eq_class);

		if (i > river_cnt)
		{
			river_cnt = i;
			selected_classes.shrink(0);
			selected_classes.add(eq_class);
			class_mask(cnt, eq_class, selected_rivers);
		}
		else
		{
			class_mask(cnt, eq_class, selected_rivers2);

			for (i = 0; i < OPT_STREAM_BITS; i++)
			{
				if ((selected_rivers[i] & selected_rivers2[i]) != selected_rivers[i])
					break;
			}

			if (i == OPT_STREAM_BITS)
				selected_classes.add(eq_class);
		}
	}

	if (!river_cnt)
		return false;

	// AB: Deactivate currently all streams from every river, because we
	// need to know which nodes are computable between the rivers used
	// for the merge.

	StreamStateHolder stateHolder(csb);
	stateHolder.deactivate();

	HalfStaticArray<RecordSource*, OPT_STATIC_ITEMS> rsbs;
	HalfStaticArray<NestValueArray*, OPT_STATIC_ITEMS> keys;

	// Unconditionally disable merge joins in favor of hash joins.
	// This is a temporary debugging measure.
	bool prefer_merge_over_hash = false;

	// AB: Get the lowest river position from the rivers that are merged

	RiverList rivers_to_merge;
	USHORT lowest_river_position = MAX_USHORT;
	USHORT number = 0;

	for (River** iter = org_rivers.begin(); iter < org_rivers.end(); number++)
	{
		River* const river = *iter;

		if (!(TEST_DEP_BIT(selected_rivers, number)))
		{
			iter++;
			continue;
		}

		if (number < lowest_river_position)
			lowest_river_position = number;

		rivers_to_merge.add(river);
		org_rivers.remove(iter);

		// Apply local river booleans, if any

		RecordSource* rsb = river->applyLocalBoolean(opt);

		// Collect RSBs and keys to join

		SortNode* const key = FB_NEW_POOL(*tdbb->getDefaultPool()) SortNode(*tdbb->getDefaultPool());

		if (prefer_merge_over_hash)
		{
			ValueExprNode*** selected_class;

			for (selected_class = selected_classes.begin();
				 selected_class != selected_classes.end(); ++selected_class)
			{
				key->direction.add(ORDER_ASC);	// Ascending sort
				key->nullOrder.add(NULLS_DEFAULT);	// Default nulls placement
				key->expressions.add((*selected_class)[number]);
			}

			StreamList streams;
			streams.assign(river->getStreams());
			rsb = OPT_gen_sort(tdbb, opt->opt_csb, streams, NULL, rsb, key, opt->favorFirstRows, false);
		}
		else
		{
			ValueExprNode*** selected_class;

			for (selected_class = selected_classes.begin();
				 selected_class != selected_classes.end(); ++selected_class)
			{
				key->expressions.add((*selected_class)[number]);
			}
		}

		// It seems that rivers are already sorted by their cardinality.
		// For a hash join, we need to choose the smallest ones as inner sub-streams,
		// hence we reverse the order when storing them in the temporary arrays.

		if (prefer_merge_over_hash)
		{
			rsbs.add(rsb);
			keys.add(&key->expressions);
		}
		else
		{
			rsbs.insert(0, rsb);
			keys.insert(0, &key->expressions);
		}
	}

	fb_assert(rsbs.getCount() == keys.getCount());

	// Build a join stream

	RecordSource* rsb = NULL;

	if (prefer_merge_over_hash)
	{
		rsb = FB_NEW_POOL(*tdbb->getDefaultPool())
			MergeJoin(csb, rsbs.getCount(), (SortedStream**) rsbs.begin(), keys.begin());
	}
	else
	{
		rsb = FB_NEW_POOL(*tdbb->getDefaultPool())
			HashJoin(tdbb, csb, rsbs.getCount(), rsbs.begin(), keys.begin());
	}

	// Activate streams of all the rivers being merged

	for (River** iter = rivers_to_merge.begin(); iter < rivers_to_merge.end(); iter++)
		(*iter)->activate(csb);

	// Pick up any boolean that may apply

	BoolExprNode* boolean = NULL;

	for (tail = opt->opt_conjuncts.begin(); tail < end; tail++)
	{
		BoolExprNode* const node = tail->opt_conjunct_node;

		if (!(tail->opt_conjunct_flags & opt_conjunct_used) &&
			!(node->nodFlags & ExprNode::FLAG_RESIDUAL) &&
			node->computable(csb, INVALID_STREAM, false))
		{
			compose(*tdbb->getDefaultPool(), &boolean, node);
			tail->opt_conjunct_flags |= opt_conjunct_used;
		}
	}

	if (boolean)
		rsb = FB_NEW_POOL(*tdbb->getDefaultPool()) FilteredStream(csb, rsb, boolean);

	River* const merged_river = FB_NEW_POOL(*tdbb->getDefaultPool()) River(csb, rsb, rivers_to_merge);

	org_rivers.insert(lowest_river_position, merged_river);

	return true;
}


static double get_cardinality(thread_db* tdbb, jrd_rel* relation, const Format* format)
{
/**************************************
 *
 *	g e t _ c a r d i n a l i t y
 *
 **************************************
 *
 * Functional description
 *	Return the estimated cardinality for
 *  the given relation.
 *
 **************************************/
	SET_TDBB(tdbb);

	if (relation->isVirtual())
	{
		// Just a dumb estimation
		return 100.0;
	}

	if (relation->rel_file)
	{
		return EXT_cardinality(tdbb, relation);
	}

	MET_post_existence(tdbb, relation);
	const double cardinality = DPM_cardinality(tdbb, relation, format);
	MET_release_existence(tdbb, relation);

	return cardinality;
}


static BoolExprNode* make_inference_node(CompilerScratch* csb, BoolExprNode* boolean,
	ValueExprNode* arg1, ValueExprNode* arg2)
{
/**************************************
 *
 *	m a k e _ i n f e r e n c e _ n o d e
 *
 **************************************
 *
 * Defined
 *	1996-Jan-15 David Schnepper
 *
 * Functional description
 *	From the predicate, boolean, and infer a new
 *	predicate using arg1 & arg2 as the first two
 *	parameters to the predicate.
 *
 *	This is used when the engine knows A<B and A=C, and
 *	creates a new node to represent the infered knowledge C<B.
 *
 *	Note that this may be sometimes incorrect with 3-value
 *	logic (per Chris Date's Object & Relations seminar).
 *	Later stages of query evaluation evaluate exactly
 *	the originally specified query, so 3-value issues are
 *	caught there.  Making this inference might cause us to
 *	examine more records than needed, but would not result
 *	in incorrect results.
 *
 *	Note that some nodes, specifically nod_like, have
 *	more than two parameters for a boolean operation.
 *	(nod_like has an optional 3rd parameter for the ESCAPE character
 *	 option of SQL)
 *	Nod_sleuth also has an optional 3rd parameter (for the GDML
 *	matching ESCAPE character language).  But nod_sleuth is
 *	(apparently) not considered during optimization.
 *
 *
 **************************************/
	thread_db* tdbb = JRD_get_thread_data();
	DEV_BLKCHK(csb, type_csb);

	ComparativeBoolNode* cmpNode = nodeAs<ComparativeBoolNode>(boolean);
	fb_assert(cmpNode);	// see our caller

	// Clone the input predicate
	ComparativeBoolNode* newCmpNode = FB_NEW_POOL(csb->csb_pool) ComparativeBoolNode(
		csb->csb_pool, cmpNode->blrOp);

	// We may safely copy invariantness flag because
	// (1) we only distribute field equalities
	// (2) invariantness of second argument of STARTING WITH or LIKE is solely
	//    determined by its dependency on any of the fields
	// If provisions above change the line below will have to be modified
	newCmpNode->nodFlags = cmpNode->nodFlags;

	// Share impure area for cached invariant value used to hold pre-compiled
	// pattern for new LIKE and CONTAINING algorithms.
	// Proper cloning of impure area for this node would require careful accounting
	// of new invariant dependencies - we avoid such hassles via using single
	// cached pattern value for all node clones. This is faster too.
	if (newCmpNode->nodFlags & ExprNode::FLAG_INVARIANT)
		newCmpNode->impureOffset = cmpNode->impureOffset;

	// But substitute new values for some of the predicate arguments
	newCmpNode->arg1 = CMP_clone_node_opt(tdbb, csb, arg1);
	newCmpNode->arg2 = CMP_clone_node_opt(tdbb, csb, arg2);

	// Arguments after the first two are just cloned (eg: LIKE ESCAPE clause)
	if (cmpNode->arg3)
		newCmpNode->arg3 = CMP_clone_node_opt(tdbb, csb, cmpNode->arg3);

	return newCmpNode;
}


static bool map_equal(const ValueExprNode* field1, const ValueExprNode* field2, const MapNode* map)
{
/**************************************
 *
 *	m a p _ e q u a l
 *
 **************************************
 *
 * Functional description
 *	Test to see if two fields are equal, where the fields
 *	are in two different streams possibly mapped to each other.
 *	Order of the input fields is important.
 *
 **************************************/
	const FieldNode* fieldNode1 = nodeAs<FieldNode>(field1);
	const FieldNode* fieldNode2 = nodeAs<FieldNode>(field2);

	if (!fieldNode1 || !fieldNode2)
		return false;

	// look through the mapping and see if we can find an equivalence.
	const NestConst<ValueExprNode>* sourcePtr = map->sourceList.begin();
	const NestConst<ValueExprNode>* targetPtr = map->targetList.begin();

	for (const NestConst<ValueExprNode>* const sourceEnd = map->sourceList.end();
		 sourcePtr != sourceEnd;
		 ++sourcePtr, ++targetPtr)
	{
		const FieldNode* mapFrom = nodeAs<FieldNode>(*sourcePtr);
		const FieldNode* mapTo = nodeAs<FieldNode>(*targetPtr);

		if (!mapFrom || !mapTo)
			continue;

		if (fieldNode1->fieldStream != mapFrom->fieldStream || fieldNode1->fieldId != mapFrom->fieldId)
			continue;

		if (fieldNode2->fieldStream != mapTo->fieldStream || fieldNode2->fieldId != mapTo->fieldId)
			continue;

		return true;
	}

	return false;
}


// Mark indices that were not included in the user-specified access plan.
static void mark_indices(CompilerScratch::csb_repeat* tail, SSHORT relationId)
{
	const PlanNode* const plan = tail->csb_plan;

	if (plan->type != PlanNode::TYPE_RETRIEVE)
		return;

	// Go through each of the indices and mark it unusable
	// for indexed retrieval unless it was specifically mentioned
	// in the plan; also mark indices for navigational access.

	// If there were none indices, this is a sequential retrieval.

	index_desc* idx = tail->csb_idx->items;

	for (USHORT i = 0; i < tail->csb_indices; i++)
	{
		if (plan->accessType)
		{
			ObjectsArray<PlanNode::AccessItem>::iterator arg = plan->accessType->items.begin();
			const ObjectsArray<PlanNode::AccessItem>::iterator end = plan->accessType->items.end();

			for (; arg != end; ++arg)
			{
				if (relationId != arg->relationId)
				{
					// index %s cannot be used in the specified plan
					ERR_post(Arg::Gds(isc_index_unused) << arg->indexName);
				}

				if (idx->idx_id == arg->indexId)
				{
					if (plan->accessType->type == PlanNode::AccessType::TYPE_NAVIGATIONAL &&
						arg == plan->accessType->items.begin())
					{
						// dimitr:	navigational access can use only one index,
						//			hence the extra check added (see the line above)
						idx->idx_runtime_flags |= idx_plan_navigate;
					}
					else
					{
						// nod_indices
						break;
					}
				}
			}

			if (arg == end)
				idx->idx_runtime_flags |= idx_plan_dont_use;
		}
		else
			idx->idx_runtime_flags |= idx_plan_dont_use;

		++idx;
	}
}


// Test two field node pointers for symbolic equality.

static bool node_equality(const ValueExprNode* node1, const ValueExprNode* node2)
{
	if (!node1 || !node2)
		return false;

	if (node1->getType() != node2->getType())
		return false;

	if (node1 == node2)
		return true;

	const FieldNode* fieldNode1 = nodeAs<FieldNode>(node1);
	const FieldNode* fieldNode2 = nodeAs<FieldNode>(node2);

	if (fieldNode1 && fieldNode2)
	{
		return fieldNode1->fieldStream == fieldNode2->fieldStream &&
			fieldNode1->fieldId == fieldNode2->fieldId;
	}

	return false;
}

static bool node_equality(const BoolExprNode* node1, const BoolExprNode* node2)
{
	DEV_BLKCHK(node1, type_nod);
	DEV_BLKCHK(node2, type_nod);

	if (!node1 || !node2)
		return false;

	if (node1->getType() != node2->getType())
		return false;

	if (node1 == node2)
		return true;

	const ComparativeBoolNode* cmpNode = nodeAs<ComparativeBoolNode>(node1);
	const ComparativeBoolNode* cmpNode2 = nodeAs<ComparativeBoolNode>(node2);

	if (cmpNode && cmpNode2 && cmpNode->blrOp == cmpNode2->blrOp &&
		(cmpNode->blrOp == blr_eql || cmpNode->blrOp == blr_equiv))
	{
		if (node_equality(cmpNode->arg1, cmpNode2->arg1) &&
			node_equality(cmpNode->arg2, cmpNode2->arg2))
		{
			return true;
		}

		if (node_equality(cmpNode->arg1, cmpNode2->arg2) &&
			node_equality(cmpNode->arg2, cmpNode2->arg1))
		{
			return true;
		}
	}

	return false;
}


static ValueExprNode* optimize_like_similar(thread_db* tdbb, CompilerScratch* csb, ComparativeBoolNode* cmpNode)
{
/**************************************
 *
 *	o p t i m i z e _ l i k e _ s i m i l a r
 *
 **************************************
 *
 * Functional description
 *	Optimize a LIKE/SIMILAR expression, if possible,
 *	into a "starting with" AND a "LIKE/SIMILAR".  This
 *	will allow us to use the index for the
 *	starting with, and the LIKE/SIMILAR can just tag
 *	along for the ride.
 *	But on the ride it does useful work, consider
 *	match LIKE/SIMILAR "ab%c".  This is optimized by adding
 *	AND starting_with "ab", but the LIKE/SIMILAR clause is
 *	still needed.
 *
 **************************************/
	SET_TDBB(tdbb);

	ValueExprNode* matchNode = cmpNode->arg1;
	ValueExprNode* patternNode = cmpNode->arg2;
	ValueExprNode* escapeNode = cmpNode->arg3;

	// if the pattern string or the escape string can't be
	// evaluated at compile time, forget it
	if (!nodeIs<LiteralNode>(patternNode) || (escapeNode && !nodeIs<LiteralNode>(escapeNode)))
		return nullptr;

	dsc matchDesc;
	matchNode->getDesc(tdbb, csb, &matchDesc);

	dsc* patternDesc = &nodeAs<LiteralNode>(patternNode)->litDesc;
	dsc* escapeDesc = nullptr;

	if (escapeNode)
		escapeDesc = &nodeAs<LiteralNode>(escapeNode)->litDesc;

	// if either is not a character expression, forget it
	if ((matchDesc.dsc_dtype > dtype_any_text) ||
		(patternDesc->dsc_dtype > dtype_any_text) ||
		(escapeNode && escapeDesc->dsc_dtype > dtype_any_text))
	{
		return nullptr;
	}

	TextType* matchTextType = INTL_texttype_lookup(tdbb, INTL_TTYPE(&matchDesc));
	CharSet* matchCharset = matchTextType->getCharSet();
	TextType* patternTextType = INTL_texttype_lookup(tdbb, INTL_TTYPE(patternDesc));
	CharSet* patternCharset = patternTextType->getCharSet();

	if (cmpNode->blrOp == blr_like)
	{
		UCHAR escape_canonic[sizeof(ULONG)];
		UCHAR first_ch[sizeof(ULONG)];
		ULONG first_len;
		UCHAR* p;
		USHORT p_count;
		MoveBuffer escapeBuffer;

		// Get the escape character, if any
		if (escapeNode)
		{
			// Ensure escape string is same character set as match string
			p_count = MOV_make_string2(tdbb, escapeDesc, INTL_TTYPE(&matchDesc), &p, escapeBuffer);

			first_len = matchCharset->substring(p_count, p, sizeof(first_ch), first_ch, 0, 1);
			matchTextType->canonical(first_len, p, sizeof(escape_canonic), escape_canonic);
		}

		MoveBuffer patternBuffer;
		p_count = MOV_make_string2(tdbb, patternDesc, INTL_TTYPE(&matchDesc), &p, patternBuffer);

		first_len = matchCharset->substring(p_count, p, sizeof(first_ch), first_ch, 0, 1);

		UCHAR first_canonic[sizeof(ULONG)];
		matchTextType->canonical(first_len, p, sizeof(first_canonic), first_canonic);

		const BYTE canWidth = matchTextType->getCanonicalWidth();

		const UCHAR* matchOneChar = matchCharset->getSqlMatchOneLength() != 0 ?
			matchTextType->getCanonicalChar(TextType::CHAR_SQL_MATCH_ONE) : nullptr;
		const UCHAR* matchAnyChar = matchCharset->getSqlMatchAnyLength() != 0 ?
			matchTextType->getCanonicalChar(TextType::CHAR_SQL_MATCH_ANY) : nullptr;

		// If the first character is a wildcard char, forget it.
		if ((!escapeNode || memcmp(first_canonic, escape_canonic, canWidth) != 0) &&
			((matchOneChar && memcmp(first_canonic, matchOneChar, canWidth) == 0) ||
			(matchAnyChar && memcmp(first_canonic, matchAnyChar, canWidth) == 0)))
		{
			return nullptr;
		}

		// allocate a literal node to store the starting with string;
		// assume it will be shorter than the pattern string

		LiteralNode* literal = FB_NEW_POOL(csb->csb_pool) LiteralNode(csb->csb_pool);
		literal->litDesc = *patternDesc;
		UCHAR* q = literal->litDesc.dsc_address = FB_NEW_POOL(csb->csb_pool) UCHAR[literal->litDesc.dsc_length];

		// Set the string length to point till the first wildcard character.

		HalfStaticArray<UCHAR, BUFFER_SMALL> patternCanonical;
		ULONG patternCanonicalLen = p_count / matchCharset->minBytesPerChar() * canWidth;

		patternCanonicalLen = matchTextType->canonical(p_count, p,
			patternCanonicalLen, patternCanonical.getBuffer(patternCanonicalLen));

		for (const UCHAR* patternPtr = patternCanonical.begin(); patternPtr < patternCanonical.end(); )
		{
			// if there are escape characters, skip past them and don't treat the next char as a wildcard
			const UCHAR* patternPtrStart = patternPtr;
			patternPtr += canWidth;

			if (escapeNode && (memcmp(patternPtrStart, escape_canonic, canWidth) == 0))
			{
				// Check for Escape character at end of string
				if (!(patternPtr < patternCanonical.end()))
					break;

				patternPtrStart = patternPtr;
				patternPtr += canWidth;
			}
			else if ((matchOneChar && memcmp(patternPtrStart, matchOneChar, canWidth) == 0) ||
					(matchAnyChar && memcmp(patternPtrStart, matchAnyChar, canWidth) == 0))
			{
				break;
			}

			q += patternCharset->substring(patternDesc->dsc_length,
					patternDesc->dsc_address,
					literal->litDesc.dsc_length - (q - literal->litDesc.dsc_address), q,
					(patternPtrStart - patternCanonical.begin()) / canWidth, 1);
		}

		literal->litDesc.dsc_length = q - literal->litDesc.dsc_address;

		return literal;
	}
	else
	{
		fb_assert(cmpNode->blrOp == blr_similar);

		MoveBuffer escapeBuffer;
		UCHAR* escapeStart = nullptr;
		ULONG escapeLen = 0;

		// Get the escape character, if any
		if (escapeNode)
		{
			// Ensure escape string is same character set as match string
			escapeLen = MOV_make_string2(tdbb, escapeDesc, INTL_TTYPE(&matchDesc), &escapeStart, escapeBuffer);
		}

		MoveBuffer patternBuffer;
		UCHAR* patternStart;
		ULONG patternLen = MOV_make_string2(tdbb, patternDesc, INTL_TTYPE(&matchDesc), &patternStart, patternBuffer);
		const auto patternEnd = patternStart + patternLen;
		const UCHAR* patternPtr = patternStart;

		MoveBuffer prefixBuffer;
		ULONG charLen = 0;

		while (IntlUtil::readOneChar(matchCharset, &patternPtr, patternEnd, &charLen))
		{
			if (escapeNode && charLen == escapeLen && memcmp(patternPtr, escapeStart, escapeLen) == 0)
			{
				if (!IntlUtil::readOneChar(matchCharset, &patternPtr, patternEnd, &charLen) ||
					!((charLen == escapeLen && memcmp(patternPtr, escapeStart, escapeLen) == 0) ||
					  (charLen == 1 && SimilarToRegex::isSpecialChar(*patternPtr))))
				{
					// Invalid escape.
					return nullptr;
				}
			}
			else if (charLen == 1 && SimilarToRegex::isSpecialChar(*patternPtr))
				break;

			prefixBuffer.push(patternPtr, charLen);
		}

		if (prefixBuffer.isEmpty())
			return nullptr;

		// Allocate a literal node to store the starting with string.
		// Use the match text type as the pattern string is converted to it.

		LiteralNode* literal = FB_NEW_POOL(csb->csb_pool) LiteralNode(csb->csb_pool);
		literal->litDesc.makeText(prefixBuffer.getCount(), INTL_TTYPE(&matchDesc),
			FB_NEW_POOL(csb->csb_pool) UCHAR[prefixBuffer.getCount()]);
		memcpy(literal->litDesc.dsc_address, prefixBuffer.begin(), prefixBuffer.getCount());

		return literal;
	}
}


static USHORT river_count(USHORT count, ValueExprNode** eq_class)
{
/**************************************
 *
 *	r i v e r _ c o u n t
 *
 **************************************
 *
 * Functional description
 *	Given an sort/merge join equivalence class (vector of node pointers
 *	of representative values for rivers), return the count of rivers
 *	with values.
 *
 **************************************/
	USHORT cnt = 0;

	for (USHORT i = 0; i < count; i++, eq_class++)
	{
		if (*eq_class)
		{
			cnt++;
			DEV_BLKCHK(*eq_class, type_nod);
		}
	}

	return cnt;
}


static bool search_stack(const ValueExprNode* node, const ValueExprNodeStack& stack)
{
/**************************************
 *
 *	s e a r c h _ s t a c k
 *
 **************************************
 *
 * Functional description
 *	Search a stack for the presence of a particular value.
 *
 **************************************/
	for (ValueExprNodeStack::const_iterator iter(stack); iter.hasData(); ++iter)
	{
		if (node_equality(node, iter.object()))
			return true;
	}

	return false;
}


static void set_direction(SortNode* fromClause, SortNode* toClause)
{
/**************************************
 *
 *	s e t _ d i r e c t i o n
 *
 **************************************
 *
 * Functional description
 *	Update the direction of a GROUP BY, DISTINCT, or ORDER BY
 *	clause to the same direction as another clause. Do the same
 *  for the nulls placement flag.
 *
 **************************************/
	const size_t fromCount = fromClause->expressions.getCount();

	fb_assert(fromCount <= toClause->expressions.getCount());
	fb_assert(fromCount == fromClause->direction.getCount() &&
		fromCount == fromClause->nullOrder.getCount());
	fb_assert(toClause->expressions.getCount() == toClause->direction.getCount() &&
		toClause->expressions.getCount() == toClause->nullOrder.getCount());

	for (FB_SIZE_T i = 0; i < fromCount; ++i)
	{
		toClause->direction[i] = fromClause->direction[i];
		toClause->nullOrder[i] = fromClause->nullOrder[i];
	}
}


static void set_position(const SortNode* from_clause, SortNode* to_clause, const MapNode* map)
{
/**************************************
 *
 *	s e t _ p o s i t i o n
 *
 **************************************
 *
 * Functional description
 *	Update the fields in a GROUP BY, DISTINCT, or ORDER BY
 *	clause to the same position as another clause, possibly
 *	using a mapping between the streams.
 *
 **************************************/
	DEV_BLKCHK(from_clause, type_nod);

	// Track the position in the from list with "to_swap", and find the corresponding
	// field in the from list with "to_ptr", then swap the two fields.  By the time
	// we get to the end of the from list, all fields in the to list will be reordered.

	NestConst<ValueExprNode>* to_swap = to_clause->expressions.begin();

	// We need to process no more than the number of nodes in the "from" clause

	const size_t count = from_clause->expressions.getCount();
	fb_assert(count <= to_clause->expressions.getCount());

	const NestConst<ValueExprNode>* from_ptr = from_clause->expressions.begin();

	for (const NestConst<ValueExprNode>* const from_end = from_ptr + count;
		 from_ptr != from_end; ++from_ptr)
	{
		NestConst<ValueExprNode>* to_ptr = to_clause->expressions.begin();

		for (const NestConst<ValueExprNode>* const to_end = to_ptr + count;
			 to_ptr != to_end; ++to_ptr)
		{
			const FieldNode* const fromField = nodeAs<FieldNode>(*from_ptr);
			const FieldNode* const toField = nodeAs<FieldNode>(*to_ptr);

			if ((map && map_equal(*to_ptr, *from_ptr, map)) ||
				(!map && fromField && toField &&
					fromField->fieldStream == toField->fieldStream &&
					fromField->fieldId == toField->fieldId))
			{
				ValueExprNode* swap = *to_swap;
				*to_swap = *to_ptr;
				*to_ptr = swap;
			}
		}

		++to_swap;
	}

}


static void sort_indices_by_selectivity(CompilerScratch::csb_repeat* tail)
{
/**************************************
 *
 *	s o r t _ i n d i c e s _ b y _ s e l e c t i v i t y
 *
 **************************************
 *
 * Functional description
 *	Sort indices based on their selectivity. Lowest selectivy as first, highest as last.
 *
 **************************************/

	index_desc* selectedIdx = NULL;
	Array<index_desc> idxSort(tail->csb_indices);
	bool sameSelectivity = false;

	// Walk through the indices and sort them into into idxSort
	// where idxSort[0] contains the lowest selectivity (best) and
	// idxSort[csbTail->csb_indices - 1] the highest (worst)

	if (tail->csb_idx && (tail->csb_indices > 1))
	{
		for (USHORT j = 0; j < tail->csb_indices; j++)
		{
			float selectivity = 1; // Maximum selectivity is 1 (when all keys are the same)
			index_desc* idx = tail->csb_idx->items;

			for (USHORT i = 0; i < tail->csb_indices; i++)
			{
				// Prefer ASC indices in the case of almost the same selectivities
				if (selectivity > idx->idx_selectivity)
					sameSelectivity = ((selectivity - idx->idx_selectivity) <= 0.00001);
				else
					sameSelectivity = ((idx->idx_selectivity - selectivity) <= 0.00001);

				if (!(idx->idx_runtime_flags & idx_marker) &&
					 (idx->idx_selectivity <= selectivity) &&
					 !((idx->idx_flags & idx_descending) && sameSelectivity))
				{
					selectivity = idx->idx_selectivity;
					selectedIdx = idx;
				}

				++idx;
			}

			// If no index was found than pick the first one available out of the list
			if ((!selectedIdx) || (selectedIdx->idx_runtime_flags & idx_marker))
			{
				idx = tail->csb_idx->items;

				for (USHORT i = 0; i < tail->csb_indices; i++)
				{
					if (!(idx->idx_runtime_flags & idx_marker))
					{
						selectedIdx = idx;
						break;
					}

					++idx;
				}
			}

			selectedIdx->idx_runtime_flags |= idx_marker;
			idxSort.add(*selectedIdx);
		}

		// Finally store the right order in cbs_tail->csb_idx
		index_desc* idx = tail->csb_idx->items;

		for (USHORT j = 0; j < tail->csb_indices; j++)
		{
			idx->idx_runtime_flags &= ~idx_marker;
			memcpy(idx, &idxSort[j], sizeof(index_desc));
			++idx;
		}
	}
}
