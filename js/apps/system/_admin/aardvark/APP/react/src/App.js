import 'jsoneditor-react/es/editor.min.css';
import React, { Component } from 'react';
import ReactDOM from 'react-dom';

const jsoneditor = require('jsoneditor');
const d3 = require('d3');
require('nvd3');

// parse prometheus
const parsePrometheusTextFormat = require('parse-prometheus-text-format');

require('../../frontend/js/arango/arangoValidationHelper.js');

// import new react views
require('./views/analyzers/AnalyzersReactView');
require('./views/graphV2/listGraphs/GraphsListReactView');
require('./views/databases/DatabasesReactView');
require('./views/graphV2/viewGraph/GraphV2ReactView');
require('./views/users/UserManagementReactView');
require('./views/views/ViewSettingsReactView');
require('./views/views/ViewsListReactView');
require('./views/collections/indices/CollectionIndicesReactView');
require('./views/query/QueryReactView');
require('./views/shards/distribution/ShardDistributionReactView');

// old libraries
const jQuery = require('jquery');

require('backbone');
const _ = require('underscore');
const Sigma = require('sigma');
const Noty = require('noty');
const Marked = require('marked');
const CryptoJS = require('crypto-js');

// highlight.js
const hljs = require('highlight.js/lib/highlight');
const json = require('highlight.js/lib/languages/json');

const env = process.env.NODE_ENV;

// import old based css files
require('../../frontend/css/pure-min.css');
require('../../frontend/ttf/arangofont/style.css');
require('../../frontend/css/bootstrap.css');
require('../../frontend/css/jquery.contextmenu.css');
require('../../frontend/css/select2.css');
require('../../frontend/css/highlightjs.css');
require('../../frontend/css/jsoneditor.css');
require('../../frontend/css/tippy.css');
require('../../frontend/css/dygraph.css');
require('leaflet/dist/leaflet.css');
require('../../frontend/css/nv.d3.css');
require('../../frontend/css/grids-responsive-min.css');

// import sass files
require('../../frontend/scss/style.scss');
require('highlight.js/styles/github.css');

// noty css
require('../node_modules/noty/lib/noty.css');
require('../node_modules/noty/lib/themes/sunset.css');

window.JST = {};

function requireAll(context) {
  context.keys().forEach(context);
  _.each(context.keys(), function (key) {
    // detect and store ejs templates
    if (key.substring(key.length - 4, key.length) === '.ejs') {
      let filename = key.substring(2, key.length);
      let name = key.substring(2, key.length - 4);
      if (env === 'development') {
        window.JST['templates/' + name] = _.template(
          require('../../frontend/js/templates/' + filename).default);
      } else {
        // production - precompiled templates
        window.JST['templates/' + name] = require('../../frontend/js/templates/' + filename);
      }
    }
  });
}

// templates ejs
requireAll(require.context(
  '../../frontend/js/templates/'
));

/**
 * `require` all backbone dependencies
 */

hljs.registerLanguage('json', json);
window.hljs = hljs;

window.Noty = Noty;

window.React = React;
window.ReactDOM = ReactDOM;
window.Joi = require('../../frontend/js/lib/joi-browser.min.js');
window.jQuery = window.$ = jQuery;
window.parsePrometheusTextFormat = parsePrometheusTextFormat;

require('../../frontend/js/lib/select2.js');
window._ = _;
require('../../frontend/js/arango/templateEngine.js');
require('../../frontend/js/arango/arango.js');

require('../../frontend/js/lib/jquery-ui-1.9.2.custom.min.js');
require('../../frontend/js/lib/jquery.form.js');
require('../../frontend/js/lib/jquery.uploadfile.min.js');
require('../../frontend/js/lib/bootstrap-min.js');

// typeahead
require('typeahead.js/dist/typeahead.jquery.min.js');
require('typeahead.js/dist/bloodhound.min.js');

// Collect all Backbone.js related
require('../../frontend/js/routers/router.js');
require('../../frontend/js/routers/versionCheck.js');
require('../../frontend/js/routers/startApp.js');


requireAll(require.context(
  '../../frontend/js/views/'
));

requireAll(require.context(
  '../../frontend/js/models/'
));

requireAll(require.context(
  '../../frontend/js/collections/'
));

// Third Party Libraries
window.tippy = require('tippy.js');
require('../../frontend/js/lib/bootstrap-pagination.min.js');
window.numeral = require('../../frontend/js/lib/numeral.min.js'); // TODO
window.JSONEditor = jsoneditor;

// ace
window.define = window.ace.define;

window.d3 = d3;

window.prettyBytes = require('../../frontend/js/lib/pretty-bytes.js');
window.Dygraph = require('../../frontend/js/lib/dygraph-combined.min.js');
require('../../frontend/js/config/dygraphConfig.js');
window.moment = require('../../frontend/js/lib/moment.min.js');

// sigma
window.sigma = Sigma;
window.marked = Marked;
window.CryptoJS = CryptoJS;

// import additional sigma plugins
require('sigma/build/plugins/sigma.layout.forceAtlas2.min'); // workaround to work with webpack

// additional sigma plugins
require('../../frontend/js/lib/sigma.canvas.edges.autoCurve.js');
require('../../frontend/js/lib/sigma.canvas.edges.curve.js');
require('../../frontend/js/lib/sigma.canvas.edges.dashed.js');
require('../../frontend/js/lib/sigma.canvas.edges.dotted.js');
require('../../frontend/js/lib/sigma.canvas.edges.labels.curve.js');
require('../../frontend/js/lib/sigma.canvas.edges.labels.curvedArrow.js');
require('../../frontend/js/lib/sigma.canvas.edges.labels.def.js');
require('../../frontend/js/lib/sigma.canvas.edges.tapered.js');
require('../../frontend/js/lib/sigma.exporters.image.js');
require('../../frontend/js/lib/sigma.layout.fruchtermanReingold.js');
require('../../frontend/js/lib/sigma.layout.noverlap.js');
require('../../frontend/js/lib/sigma.plugins.animate.js');
require('../../frontend/js/lib/sigma.plugins.dragNodes.js');
require('../../frontend/js/lib/sigma.plugins.filter.js');
require('../../frontend/js/lib/sigma.plugins.fullScreen.js');
require('../../frontend/js/lib/sigma.plugins.lasso.js');
require('../../frontend/js/lib/sigma.renderers.halo.js');

require('../../frontend/js/lib/wheelnav.slicePath.js');
require('../../frontend/js/lib/wheelnav.min.js');
window.Raphael = require('../../frontend/js/lib/raphael.min.js');
window.icon = require('../../frontend/js/lib/raphael.icons.min.js');

window.randomColor = require('../../frontend/js/lib/randomColor.js');
// require('../../frontend/src/ace.js');
// require('../../frontend/src/theme-textmate.js');
// require('../../frontend/src/mode-json.js');
// require('../../frontend/src/mode-aql.js');

class App extends Component {
  render() {
    return (
      <div className="App" />
    );
  }
}

export default App;
