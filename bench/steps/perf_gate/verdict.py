# The exceptions of a run and the statistics that turn paired timings into a verdict.
# interval() and verdict() decide pass, suspect, regression or inconclusive; no server is needed.

class Inconclusive(Exception):
    pass


class BudgetExpired(Inconclusive):
    pass


class PlanAbsent(ValueError):
    pass


def interval(pairs):
    if len(pairs) not in T95:
        raise ValueError('a block requires exactly 6 or 16 independent pairs')
    if any(not math.isfinite(x) or x <= 0 for p in pairs for x in p):
        raise ValueError('invalid elapsed sample')
    logs = [math.log(b / a) for a, b in pairs]
    mu = statistics.mean(logs)
    half = T95[len(pairs)] * statistics.stdev(logs) / math.sqrt(len(logs))
    return dict(n=len(logs), degrees_freedom=len(logs)-1, t_critical=T95[len(logs)], ratio=math.exp(mu), lower=math.exp(mu-half),
                upper=math.exp(mu+half), log_sd=statistics.stdev(logs),
                baseline_median=statistics.median(a for a, _ in pairs),
                candidate_median=statistics.median(b for _, b in pairs),
                baseline_range=[min(a for a, _ in pairs), max(a for a, _ in pairs)],
                candidate_range=[min(b for _, b in pairs), max(b for _, b in pairs)])


def verdict(block, confirmation=False):
    if block['upper'] <= MATERIAL:
        return 'pass'
    if confirmation and block['lower'] > MATERIAL:
        return 'regression'
    return 'inconclusive' if confirmation else 'suspect'


def extension_fits(confirmation, deadline, reserve):
    cost = EXTENSION_PAIRS * confirmation['runtime_seconds'] / confirmation['n']
    return dict(fits=deadline - time.monotonic() > cost + reserve, predicted_seconds=cost, reserve_seconds=reserve)


def require_equal(left, right, label):
    if left != right:
        raise ValueError(f'{label}: correctness differs: {left!r} != {right!r}')
