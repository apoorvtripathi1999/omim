#include "openlr/paths_connector.hpp"
#include "openlr/helpers.hpp"

#include "base/checked_cast.hpp"
#include "base/stl_iterator.hpp"

#include <algorithm>
#include <map>
#include <numeric>
#include <queue>
#include <tuple>

using namespace std;

namespace openlr
{
namespace
{
size_t IntersectionLen(Graph::EdgeVector a, Graph::EdgeVector b)
{
  sort(begin(a), end(a));
  sort(begin(b), end(b));
  return set_intersection(begin(a), end(a), begin(b), end(b), CounterIterator()).GetCount();
}

bool PrefEqualsSuff(Graph::EdgeVector const & a, Graph::EdgeVector const & b, size_t const len)
{
  ASSERT_LESS_OR_EQUAL(len, a.size(), ());
  ASSERT_LESS_OR_EQUAL(len, b.size(), ());
  return equal(end(a) - len, end(a), begin(b));
}

// Returns a length of the longest suffix of |a| that matches any prefix of |b|.
// Neither |a| nor |b| can contain several repetitions of any edge.
// Returns -1 if |a| intersection |b| is not equal to some suffix of |a| and some prefix of |b|.
int32_t PathOverlappingLen(Graph::EdgeVector const & a, Graph::EdgeVector const & b)
{
  auto const len = IntersectionLen(a, b);
  if (PrefEqualsSuff(a, b, len))
    return base::checked_cast<int32_t>(len);
  return -1;
}

bool ValidatePath(Graph::EdgeVector const & path,
                  uint32_t const distanceToNextPoint,
                  double const pathLengthTolerance)
{

  double pathLen = 0.0;
  for (auto const & e : path)
    pathLen += EdgeLength(e);

  double pathDiffPercent = AbsDifference(static_cast<double>(distanceToNextPoint), pathLen) /
                           static_cast<double>(distanceToNextPoint);

  LOG(LDEBUG, ("Validating path:", LogAs2GisPath(path)));

  if (pathDiffPercent > pathLengthTolerance)
  {
    LOG(LDEBUG,
        ("Shortest path doest not meet required length constraints, error:", pathDiffPercent));
    return false;
  }

  return true;
}
}  // namespace

PathsConnector::PathsConnector(double const pathLengthTolerance, Graph const & graph,
                              v2::Stats & stat)
  : m_pathLengthTolerance(pathLengthTolerance), m_graph(graph), m_stat(stat)
{
}

bool PathsConnector::ConnectCandidates(vector<LocationReferencePoint> const & points,
                                       vector<vector<Graph::EdgeVector>> const & lineCandidates,
                                       vector<Graph::EdgeVector> & resultPath)
{
  ASSERT(!points.empty(), ());

  resultPath.resize(points.size() - 1);

  // TODO(mgsergio): Discard last point on failure.
  // TODO(mgsergio): Do not iterate more than kMaxRetries times.
  // TODO(mgserigio): Make kMaxRetries depend on points number in the segment.
  for (size_t i = 1; i < points.size(); ++i)
  {
    bool found = false;

    auto const & point = points[i - 1];
    auto const distanceToNextPoint = point.m_distanceToNextPoint;
    auto const & fromCandidates = lineCandidates[i - 1];
    auto const & toCandidates = lineCandidates[i];
    auto & resultPathPart = resultPath[i - 1];

    Graph::EdgeVector fakePath;

    for (size_t fromInd = 0; fromInd < fromCandidates.size() && !found; ++fromInd)
    {
      for (size_t toInd = 0; toInd < toCandidates.size() && !found; ++toInd)
      {
        resultPathPart.clear();

        found = ConnectAdjacentCandidateLines(fromCandidates[fromInd], toCandidates[toInd],
                                              point.m_functionalRoadClass, distanceToNextPoint,
                                              resultPathPart);

        if (!found)
          continue;

        found = ValidatePath(resultPathPart, distanceToNextPoint, m_pathLengthTolerance);
        if (fakePath.empty() && found &&
            (resultPathPart.front().IsFake() || resultPathPart.back().IsFake()))
        {
          fakePath = resultPathPart;
          found = false;
        }
      }
    }

    if (!fakePath.empty() && !found)
    {
      found = true;
      resultPathPart = fakePath;
    }

    if (!found)
    {
      LOG(LDEBUG, ("No shortest path found"));
      ++m_stat.m_noShortestPathFound;
      resultPathPart.clear();
      return false;
    }
  }

  ASSERT_EQUAL(resultPath.size(), points.size() - 1, ());

  return true;
}

bool PathsConnector::FindShortestPath(Graph::Edge const & from, Graph::Edge const & to,
                                      FunctionalRoadClass const frc, uint32_t const maxPathLength,
                                      Graph::EdgeVector & path)
{
  // TODO(mgsergio): Turn Dijkstra to A*.

  uint32_t const kLengthToleranceM = 10;

  struct State
  {
    State(Graph::Edge const & e, uint32_t const s) : m_edge(e), m_score(s) {}

    bool operator>(State const & o) const
    {
      return make_tuple(m_score, m_edge) > make_tuple(o.m_score, o.m_edge);
    }

    Graph::Edge m_edge;
    uint32_t m_score;
  };

  priority_queue<State, vector<State>, greater<State>> q;
  map<Graph::Edge, uint32_t> scores;
  map<Graph::Edge, Graph::Edge> links;

  q.emplace(from, 0);
  scores[from] = 0;

  while (!q.empty())
  {
    auto const state = q.top();
    q.pop();

    auto const & u = state.m_edge;
    // TODO(mgsergio): Unify names: use either score or distance.
    auto const us = state.m_score;

    if (us > maxPathLength + kLengthToleranceM)
      continue;

    if (us > scores[u])
      continue;

    if (u == to)
    {
      for (auto e = u; e != from; e = links[e])
        path.push_back(e);
      path.push_back(from);
      reverse(begin(path), end(path));
      return true;
    }

    Graph::EdgeVector edges;
    m_graph.GetOutgoingEdges(u.GetEndJunction(), edges);
    for (auto const & e : edges)
    {
      // TODO(mgsergio): Use frc to filter edges.

      auto const it = scores.find(e);
      auto const eScore = us + EdgeLength(e);
      if (it == end(scores) || it->second > eScore)
      {
        scores[e] = eScore;
        links[e] = u;
        q.emplace(e, eScore);
      }
    }
  }

  return false;
}

bool PathsConnector::ConnectAdjacentCandidateLines(Graph::EdgeVector const & from,
                                                   Graph::EdgeVector const & to,
                                                   FunctionalRoadClass const frc,
                                                   uint32_t const distanceToNextPoint,
                                                   Graph::EdgeVector & resultPath)

{
  ASSERT(!to.empty(), ());

  if (auto const skip = PathOverlappingLen(from, to))
  {
    if (skip == -1)
      return false;
    copy(begin(from), end(from), back_inserter(resultPath));
    copy(begin(to) + skip, end(to), back_inserter(resultPath));
    return true;
  }

  ASSERT(from.back() != to.front(), ());

  Graph::EdgeVector shortestPath;
  auto const found =
      FindShortestPath(from.back(), to.front(), frc, distanceToNextPoint, shortestPath);
  if (!found)
    return false;

  // Skip the last edge from |from| because it already took its place at begin(shortestPath).
  copy(begin(from), prev(end(from)), back_inserter(resultPath));
  copy(begin(shortestPath), end(shortestPath), back_inserter(resultPath));
  // Skip the first edge from |to| because it already took its place at prev(end(shortestPath)).
  copy(next(begin(to)), end(to), back_inserter(resultPath));

  return found;
}
}  // namespace openlr
