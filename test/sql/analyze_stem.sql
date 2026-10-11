CREATE EXTENSION sift;

-- Snowball English (Porter2), from the published algorithm's examples
SELECT w, (SELECT string_agg(token, ' ') FROM sift.tokenize(w, stemmer => 'en')) AS stem
  FROM unnest(ARRAY[
    'consign', 'consigned', 'consigning', 'consignment', 'consist', 'consisted', 'consistency',
    'consistent', 'consistently', 'consisting', 'consists', 'consolation', 'consolations',
    'consolatory', 'console', 'consoled', 'consoles', 'consolidate', 'consolidated',
    'consolidating', 'consoling', 'consolingly', 'consols', 'consonant', 'consort', 'consorted',
    'consorting', 'conspicuous', 'conspicuously', 'conspiracy', 'conspirator', 'conspirators',
    'conspire', 'conspired', 'conspiring', 'constable', 'constables', 'constance', 'constancy',
    'constant', 'knack', 'knackeries', 'knacks', 'knag', 'knave', 'knaves', 'knavish', 'kneaded',
    'kneading', 'knee', 'kneel', 'kneeled', 'kneeling', 'kneels', 'knees', 'knell', 'knelt',
    'knew', 'knick', 'knif', 'knife', 'knight', 'knightly', 'knights', 'knit', 'knits',
    'knitted', 'knitting', 'knives', 'knob', 'knobs', 'knock', 'knocked', 'knocker', 'knockers',
    'knocking', 'knocks', 'knopp', 'knot', 'knots']) w;

-- exceptions, short words, regions and special prefixes
SELECT w, (SELECT string_agg(token, ' ') FROM sift.tokenize(w, stemmer => 'en')) AS stem
  FROM unnest(ARRAY[
    'skies', 'skis', 'dying', 'lying', 'tying', 'news', 'atlas', 'andes', 'only', 'early',
    'gently', 'idly', 'singly', 'ugly', 'howe', 'cosmos', 'bias', 'sky', 'is', 'as', 'by', 'a',
    'proceed', 'exceed', 'succeed', 'proceeding', 'agreed', 'feed', 'inning', 'outing',
    'canning', 'herring', 'earring', 'evening', 'generous', 'generously', 'communication',
    'community', 'arsenal', 'university', 'universal', 'organization', 'international',
    'emergency', 'lateral', 'pasted', 'pasting', 'caresses', 'ponies', 'ties', 'cats', 'gas',
    'running', 'hopping', 'hoping', 'filing', 'luxuriating', 'happy', 'say', 'cry', 'yelling',
    'sayings', 'relational', 'conditional', 'rational', 'valenci', 'hesitanci', 'digitizer',
    'conformabli', 'radicalli', 'differentli', 'vileli', 'analogousli', 'vietnamization',
    'predication', 'operator', 'feudalism', 'decisiveness', 'hopefulness', 'callousness',
    'formaliti', 'sensitiviti', 'sensibiliti', 'triplicate', 'formative', 'formalize',
    'electriciti', 'electrical', 'hopeful', 'goodness', 'revival', 'allowance', 'inference',
    'airliner', 'gyroscopic', 'adjustable', 'defensible', 'irritant', 'replacement',
    'adjustment', 'dependent', 'adoption', 'homologou', 'communism', 'activate', 'angulariti',
    'homologous', 'effective', 'bowdlerize', 'probate', 'rate', 'cease', 'controll', 'roll',
    'geology', 'biology', 'ecologist', 'fully', 'helplessly', 'abruptly', 'year1990s', '1990s']) w;

-- only lowercase ASCII tokens are stemmed; folding happens first
SELECT sift.tokenize('Running RUNNING Naïve ponies', stemmer => 'en');
SELECT sift.tokenize('Running ponies', stemmer => 'en', case_folding => false);
SELECT sift.tokenize('café cafés', stemmer => 'english', accent_folding => false);
SELECT count(*) FROM sift.tokenize('running', stemmer => 'none');
SELECT count(*) FROM sift.tokenize('running', stemmer => '');

SELECT * FROM sift.tokenize('x', stemmer => 'klingon');

DROP EXTENSION sift;
