#define CMR_DEBUG /** Uncomment to debug this file. */

#include "env_internal.h"
#include "regularity_internal.h"

#include <time.h>

CMR_ERROR CMRregularityTaskCreateRoot(CMR* cmr, CMR_MATROID_DEC* dec, DecompositionTask** ptask,
  CMR_REGULAR_PARAMS* params, CMR_REGULAR_STATS* stats, clock_t startClock, double timeLimit)
{
  assert(cmr);
  assert(dec);
  assert(ptask);
  assert(params);

  CMR_CALL( CMRallocBlock(cmr, ptask) );
  DecompositionTask* task = *ptask;

  task->dec = dec;
  task->next = NULL;

  task->params = params;
  task->stats = stats;
  task->startClock = startClock;
  task->timeLimit = timeLimit;

  return CMR_OKAY;
}

CMR_ERROR CMRregularityTaskFree(CMR* cmr, DecompositionTask** ptask)
{
  assert(cmr);
  assert(ptask);

  if (*ptask == NULL)
    return CMR_OKAY;

  CMR_CALL( CMRfreeBlock(cmr, ptask) );

  return CMR_OKAY;
}

CMR_ERROR CMRregularityQueueCreate(CMR* cmr, DecompositionQueue** pqueue)
{
  assert(cmr);
  assert(pqueue);

  CMR_CALL( CMRallocBlock(cmr, pqueue) );
  DecompositionQueue* queue = *pqueue;
  queue->head = NULL;
  queue->foundIrregularity = false;

  return CMR_OKAY;
}

CMR_ERROR CMRregularityQueueFree(CMR* cmr, DecompositionQueue** pqueue)
{
  assert(cmr);
  assert(pqueue);

  DecompositionQueue* queue = *pqueue;
  if (queue == NULL)
    return CMR_OKAY;

  while (queue->head)
  {
    DecompositionTask* task = queue->head;
    queue->head = task->next;
    CMR_CALL( CMRregularityTaskFree(cmr, &task) );
  }

  CMR_CALL( CMRfreeBlock(cmr, pqueue) );

  return CMR_OKAY;
}

bool CMRregularityQueueEmpty(DecompositionQueue* queue)
{
  assert(queue);

  return queue->head == NULL;
}

DecompositionTask* CMRregularityQueueRemove(DecompositionQueue* queue)
{
  assert(queue);

  DecompositionTask* task = queue->head;
  queue->head = task->next;
  task->next = NULL;
  return task;
}

void CMRregularityQueueAdd(DecompositionQueue* queue, DecompositionTask* task)
{
  assert(queue);

  task->next = queue->head;
  queue->head = task;
}

/**
 * \brief Runs a task for processing the associated decomposition node.
 */

static
CMR_ERROR CMRregularityTaskRun(
  CMR* cmr,                 /**< \ref CMR environment. */
  DecompositionTask* task,  /**< Task to be processed; already removed from the list of unprocessed tasks. */
  DecompositionQueue* queue /**< Queue of unprocessed tasks. */
)
{
  assert(cmr);
  assert(task);
  assert(queue);

  CMRdbgMsg(2, "Processing %p.\n", task);

  if (!task->dec->testedTwoConnected)
  {
    CMRdbgMsg(4, "Searching for 1-separations.\n");
    CMR_CALL( CMRregularitySearchOneSum(cmr, task, queue) );
  }
  else if (!task->dec->graphicness
    && (task->params->directGraphicness || task->dec->matrix->numRows <= 3 || task->dec->matrix->numColumns <= 3))
  {
    CMRdbgMsg(4, "Testing directly for %s.\n", task->dec->isTernary ? "being network" : "graphicness");
    CMR_CALL( CMRregularityTestGraphicness(cmr, task, queue) );
  }
  else if (!task->dec->cographicness
    && (task->params->directGraphicness || task->dec->matrix->numRows <= 3 || task->dec->matrix->numColumns <= 3))
  {
    CMRdbgMsg(4, "Testing directly for %s.\n", task->dec->isTernary ? "being conetwork" : "cographicness");
    CMR_CALL( CMRregularityTestCographicness(cmr, task, queue) );
  }
  else if (!task->dec->testedR10)
  {
    CMRdbgMsg(4, "Testing for being R_10.\n");
    CMR_CALL( CMRregularityTestR10(cmr, task, queue) );
  }
  else if (!task->dec->testedSeriesParallel)
  {
    CMRdbgMsg(4, "Testing for series-parallel reductions.\n");
    CMR_CALL( CMRregularityDecomposeSeriesParallel(cmr, task, queue) );
  }
  else if (task->dec->denseMatrix)
  {
    CMRdbgMsg(4, "Attempting to construct a sequence of nested minors.\n");
    CMR_CALL( CMRregularityExtendNestedMinorSequence(cmr, task, queue) );
  }
  else if (task->dec->nestedMinorsMatrix && (task->dec->nestedMinorsLastGraphic == SIZE_MAX))
  {
    CMRdbgMsg(4, "Testing along the sequence for %s.\n", task->dec->isTernary ? "being network" : "graphicness");
    CMR_CALL( CMRregularityNestedMinorSequenceGraphicness(cmr, task, queue) );
  }
  else if (task->dec->nestedMinorsMatrix && (task->dec->nestedMinorsLastCographic == SIZE_MAX))
  {
    CMRdbgMsg(4, "Testing along the sequence for %s.\n", task->dec->isTernary ? "being conetwork" : "cographicness");
    CMR_CALL( CMRregularityNestedMinorSequenceCographicness(cmr, task, queue) );
  }
  else
  {
    CMRdbgMsg(4, "Searching for 3-separations along the sequence.\n");
    CMR_CALL( CMRregularityNestedMinorSequenceSearchThreeSeparation(cmr, task, queue) );
  }

  return CMR_OKAY;
}

CMR_ERROR CMRregularityTest(CMR* cmr, CMR_CHRMAT* matrix, bool ternary, bool *pisRegular, CMR_MATROID_DEC** pdec,
  CMR_MINOR** pminor, CMR_REGULAR_PARAMS* params, CMR_REGULAR_STATS* stats, double timeLimit)
{
  assert(cmr);
  assert(matrix);
  assert(params);

#if defined(CMR_DEBUG)
  CMRdbgMsg(0, "Testing a %s %zux%zu matrix for regularity.\n", ternary ? "ternary" : "binary", matrix->numRows,
    matrix->numColumns);
  CMR_CALL( CMRchrmatPrintDense(cmr, matrix, stdout, '0', false) );
#endif /* CMR_DEBUG */

  clock_t time = clock();
  if (stats)
    stats->totalCount++;

  CMR_MATROID_DEC* root = NULL;
  CMR_CALL( CMRmatroiddecCreateMatrixRoot(cmr, &root, ternary, matrix) );
  assert(root);

  DecompositionQueue* queue = NULL;
  CMR_CALL( CMRregularityQueueCreate(cmr, &queue) );
  DecompositionTask* rootTask = NULL;
  CMR_CALL( CMRregularityTaskCreateRoot(cmr, root, &rootTask, params, stats, time, timeLimit) );
  CMRregularityQueueAdd(queue, rootTask);

  while (!CMRregularityQueueEmpty(queue) && (params->completeTree || !queue->foundIrregularity))
  {
    DecompositionTask* task = CMRregularityQueueRemove(queue);
    CMR_CALL( CMRregularityTaskRun(cmr, task, queue) );
  }

  CMR_CALL( CMRregularityQueueFree(cmr, &queue) );

  CMR_CALL( CMRmatroiddecSetAttributes(root) );
  assert(root->regularity != 0);
  if (pisRegular)
    *pisRegular = root->regularity > 0;

  /* Either store or free the decomposition. */
  if (pdec)
    *pdec = root;
  else
    CMR_CALL( CMRmatroiddecFree(cmr, &root) );

  if (stats)
    stats->totalTime += (clock() - time) * 1.0 / CLOCKS_PER_SEC;

  return CMR_OKAY;
}

CMR_ERROR CMRregularityCompleteDecomposition(CMR* cmr, CMR_MATROID_DEC* dec, CMR_REGULAR_PARAMS* params,
  CMR_REGULAR_STATS* stats, double timeLimit)
{
  assert(cmr);
  assert(dec);
  assert(params);

  /* Compute the root of the decomposition tree. */
  CMR_MATROID_DEC* root = dec;
  while (root->parent != NULL)
    root = root->parent;

#if defined(CMR_DEBUG)
  CMRdbgMsg(0, "Completing decomposition tree for a %s %zux%zu matrix.\n",
    dec->isTernary ? "ternary" : "binary", root->matrix->numRows, root->matrix->numColumns);
  CMRdbgMsg(0, "Considered subtree belongs to the %zux%zu matrix.\n",
    dec->matrix->numRows, dec->matrix->numColumns);
  CMR_CALL( CMRchrmatPrintDense(cmr, dec->matrix, stdout, '0', false) );
#endif /* CMR_DEBUG */

  clock_t time = clock();
  if (stats)
    stats->totalCount++;

  for (size_t c = 0; c < dec->numChildren; ++c)
    CMR_CALL( CMRmatroiddecFree(cmr, &dec->children[c]) );

  dec->type = CMR_MATROID_DEC_TYPE_UNKNOWN;

  DecompositionQueue* queue = NULL;
  CMR_CALL( CMRregularityQueueCreate(cmr, &queue) );
  DecompositionTask* decTask = NULL;
  CMR_CALL( CMRregularityTaskCreateRoot(cmr, dec, &decTask, params, stats, time, timeLimit) );
  CMRregularityQueueAdd(queue, decTask);

  while (!CMRregularityQueueEmpty(queue) && (params->completeTree || !queue->foundIrregularity))
  {
    DecompositionTask* task = CMRregularityQueueRemove(queue);
    CMR_CALL( CMRregularityTaskRun(cmr, task, queue) );
  }

  CMR_CALL( CMRregularityQueueFree(cmr, &queue) );

  CMR_CALL( CMRmatroiddecSetAttributes(root) );
  assert(root->regularity != 0);

  if (stats)
    stats->totalTime += (clock() - time) * 1.0 / CLOCKS_PER_SEC;

  return CMR_OKAY;
}

