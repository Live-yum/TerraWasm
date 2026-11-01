import { parse } from './vendor/acorn/acorn.mjs'

const fail = reason => { throw new Error(`BASIC loader template drift: native export aliases (${reason})`) }
const identifier = (node, name) => node?.type === 'Identifier' && node.name === name
const member = (node, object) => node?.type === 'MemberExpression' && !node.optional
  && identifier(node.object, object) && node.computed && node.property.type === 'Literal'
  && typeof node.property.value === 'string'

function walk(node, visit) {
  if (!node || typeof node !== 'object') return
  if (node.type) visit(node)
  for (const value of Object.values(node)) {
    if (Array.isArray(value)) for (const child of value) walk(child, visit)
    else if (value && typeof value === 'object') walk(value, visit)
  }
}

/** Remove only unread native-export aliases, never Module exports. The proof is
 * deliberately stricter than a general optimizer: a candidate has exactly one
 * declaration in the factory and one discarded assignment in its direct child
 * assignWasmExports. Any other Identifier occurrence anywhere in the AST,
 * including a nested read or shadow declaration, rejects the transformation.
 * Unique declaration plus these explicit ancestor scopes resolves the binding
 * without guessing about source spelling, comments or minifier naming. */
export function pruneNativeExportAliases(source) {
  const ast = parse(source, { ecmaVersion: 2022, sourceType: 'script' })
  const scope = ast.body[1]
  const binding = scope?.body?.[1]?.expression
  const sequence = binding?.right
  const factory = sequence?.expressions?.[1]
  if (ast.body[0]?.type !== 'VariableDeclaration' || ast.body[0].kind !== 'var'
    || ast.body[0].declarations.length !== 1 || !identifier(ast.body[0].declarations[0].id, 'TerraWorldWasmWeb')
    || scope?.type !== 'BlockStatement' || scope.body.length !== 2
    || scope.body[0].type !== 'VariableDeclaration' || scope.body[0].kind !== 'let'
    || scope.body[0].declarations.length !== 1 || !identifier(scope.body[0].declarations[0].id, '_scriptName')
    || binding?.type !== 'AssignmentExpression' || binding.operator !== '=' || !identifier(binding.left, 'TerraWorldWasmWeb')
    || sequence?.type !== 'SequenceExpression' || sequence.expressions.length !== 2
    || sequence.expressions[0].type !== 'Literal' || sequence.expressions[0].value !== 0
    || factory?.type !== 'FunctionExpression' || !factory.async || factory.generator) fail('factory scope')

  const identifiers = new Map()
  walk(ast, node => {
    if (node.type === 'WithStatement') fail('dynamic lexical scope')
    if (node.type === 'Identifier') {
      if (['eval', 'Function'].includes(node.name)) fail('dynamic code')
      const uses = identifiers.get(node.name) || []
      uses.push(node); identifiers.set(node.name, uses)
    }
    if (node.type === 'MemberExpression') {
      const name = node.computed ? node.property.value : node.property.name
      if (['eval', 'Function', 'constructor', 'construct'].includes(name)) fail('dynamic code')
    }
  })
  const helpers = factory.body.body.filter(node => node.type === 'FunctionDeclaration'
    && identifier(node.id, 'assignWasmExports'))
  const moduleBindings = factory.body.body.flatMap(node => node.type === 'VariableDeclaration' ? node.declarations : [])
    .filter(node => identifier(node.id, 'Module'))
  if (helpers.length !== 1 || moduleBindings.length !== 1 || !identifier(moduleBindings[0].init, 'moduleArg')) fail('export owner')
  const helper = helpers[0]
  if (helper.params.length !== 1 || !identifier(helper.params[0], 'wasmExports')) fail('export parameter')
  // No helper-local declaration can shadow the factory's Module or aliases.
  if (helper.body.body.some(node => node.type !== 'ExpressionStatement'
    || node.expression.type !== 'AssignmentExpression' || node.expression.operator !== '=')) fail('export statement shape')

  const declarations = new Map()
  for (const statement of factory.body.body) {
    if (statement.type !== 'VariableDeclaration' || statement.kind !== 'var') continue
    for (const declaration of statement.declarations) {
      if (declaration.id.type === 'Identifier' && declaration.init === null) {
        if (declarations.has(declaration.id.name)) fail('duplicate declaration')
        declarations.set(declaration.id.name, { declaration, statement })
      }
    }
  }
  const removed = new Set(), groups = new Set(), edits = []
  for (const statement of helper.body.body) {
    const outer = statement.expression, inner = outer.right
    if (outer.left.type !== 'Identifier' || !outer.left.name.startsWith('_')
      || inner?.type !== 'AssignmentExpression' || inner.operator !== '=' || !member(inner.left, 'Module')
      || inner.left.property.value !== outer.left.name || !member(inner.right, 'wasmExports')) continue
    const name = outer.left.name, entry = declarations.get(name), uses = identifiers.get(name)
    if (!entry || uses?.length !== 2 || !uses.includes(entry.declaration.id) || !uses.includes(outer.left)) fail(`read or shadow: ${name}`)
    if (removed.has(name)) fail(`duplicate assignment: ${name}`)
    removed.add(name); groups.add(entry.statement)
    // Keep the complete inner Module assignment verbatim and in place. Its
    // setters, native function identity, exceptions and ordering are unchanged.
    edits.push({ start: outer.start, end: inner.start, text: '' })
  }
  if (!removed.size) fail('no provably unread aliases')
  for (const statement of groups) {
    const kept = statement.declarations.filter(node => !removed.has(node.id.name))
    edits.push({ start: statement.start, end: statement.end,
      text: kept.length ? `var ${kept.map(node => source.slice(node.start, node.end)).join(',')};` : '' })
  }
  let end = source.length
  for (const edit of edits.sort((left, right) => right.start - left.start)) {
    if (edit.end > end) fail('overlapping edits')
    source = source.slice(0, edit.start) + edit.text + source.slice(edit.end)
    end = edit.start
  }
  return source
}
